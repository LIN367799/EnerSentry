// sim/modbus_tcp_server.cpp —— B3 最小 TCP 从站实现（ENS-SIM-IMP §6.1）。
// 注意跨平台 socket 差异：Windows 需 WSAStartup + closesocket，POSIX 用 close。
#include "sim/modbus_tcp_server.h"

#include "core/mbap.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <utility>

#ifdef _WIN32
  #define NOMINMAX
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

namespace ens::sim {

namespace {

bool ensureWinsock() noexcept {
#ifdef _WIN32
    static const bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

void closeFd(int fd) noexcept {
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
}

// 循环收发，处理部分读/写（返回 false 表示连接已断/错误）
bool recvAll(int fd, uint8_t* buf, size_t n) noexcept {
    size_t got = 0;
    while (got < n) {
#ifdef _WIN32
        const int r = ::recv(fd, reinterpret_cast<char*>(buf + got),
                             static_cast<int>(n - got), 0);
#else
        const ssize_t r = ::recv(fd, buf + got, n - got, 0);
#endif
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

bool sendAll(int fd, const uint8_t* buf, size_t n) noexcept {
    size_t sent = 0;
    while (sent < n) {
#ifdef _WIN32
        const int w = ::send(fd, reinterpret_cast<const char*>(buf + sent),
                             static_cast<int>(n - sent), 0);
#else
        const ssize_t w = ::send(fd, buf + sent, n - sent, 0);
#endif
        if (w <= 0) return false;
        sent += static_cast<size_t>(w);
    }
    return true;
}

}  // namespace

ModbusTcpServer::ModbusTcpServer(std::string ip, uint16_t port)
    : m_ip(std::move(ip)), m_port(port) {}

ModbusTcpServer::~ModbusTcpServer() { close(); }

bool ModbusTcpServer::open() noexcept {
    if (!ensureWinsock()) return false;
    if (m_listenFd >= 0) return true;               // 已打开：幂等

    // bind 失败（端口占用）→ port+1 重试 ≤3 次（ENS-LLD-SIM §6.1）
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        if (fd < 0) return false;

        int opt = 1;
#ifdef _WIN32
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_port);
        if (::inet_pton(AF_INET, m_ip.c_str(), &addr.sin_addr) != 1) {
            closeFd(fd);
            return false;
        }

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
            ::listen(fd, 16) == 0) {
            // port=0（OS 分配）时回填实际端口，供 actualPort()/客户端连接使用
            sockaddr_in bound{};
#ifdef _WIN32
            int boundLen = static_cast<int>(sizeof(bound));
#else
            socklen_t boundLen = sizeof(bound);
#endif
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0)
                m_port = ntohs(bound.sin_port);
            m_listenFd = fd;
            m_running = true;
            m_acceptThread = std::thread(&ModbusTcpServer::acceptLoop, this);
            return true;
        }
        closeFd(fd);
        if (m_port == 65535) return false;          // 端口域耗尽
        ++m_port;                                   // 重试 port+1
    }
    return false;
}

void ModbusTcpServer::close() noexcept {
    if (!m_running && m_listenFd < 0) return;       // 幂等
    m_running = false;
    dropAllClients();                               // 强制断开已接受的连接（client 收 FIN）
    if (m_listenFd >= 0) {
        closeFd(m_listenFd);
        m_listenFd = -1;
    }
    if (m_acceptThread.joinable()) m_acceptThread.join();
}

void ModbusTcpServer::dropAllClients() noexcept {
    std::lock_guard<std::mutex> lock(m_clientsMtx);
    for (const int fd : m_clients) closeFd(fd);
    m_clients.clear();
}

void ModbusTcpServer::setRequestHandler(RequestHandler cb) {
    m_handler = std::move(cb);
}

void ModbusTcpServer::boostThreadPriority() noexcept {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);   // 失败忽略
#else
    if (nice(-10) == -1) { /* EPERM 忽略：非 root 尽力而为 */ }
#endif
}

void ModbusTcpServer::acceptLoop() noexcept {
    while (m_running) {
        sockaddr_in clientAddr{};
#ifdef _WIN32
        int addrLen = static_cast<int>(sizeof(clientAddr));
#else
        socklen_t addrLen = sizeof(clientAddr);
#endif
        const int clientFd = static_cast<int>(::accept(
            m_listenFd, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen));
        if (clientFd < 0) {
            if (!m_running) break;                  // close() 关 listen 后正常退出
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(m_clientsMtx);
            m_clients.push_back(clientFd);
        }
        std::thread(&ModbusTcpServer::clientLoop, this, clientFd).detach();
    }
}

void ModbusTcpServer::clientLoop(int clientFd) noexcept {
    boostThreadPriority();

    std::array<uint8_t, 7> mbap{};
    std::array<uint8_t, 255> pdu{};                 // 最大 PDU（254 数据字节 + fc）

    while (m_running) {
        if (!recvAll(clientFd, mbap.data(), mbap.size())) break;

        core::MbapHeader hdr;
        if (!core::parse_mbap(mbap.data(), mbap.size(), hdr)) break;      // protocolId!=0 或长度异常
        // MBAP length = unitId(1) + PDU 字节数；unitId 已随 7B 头读走，PDU 只需再读 length-1
        if (hdr.length < 2 || static_cast<size_t>(hdr.length - 1) > pdu.size()) break;
        const size_t pduLen = static_cast<size_t>(hdr.length - 1);        // 脏长度 → 丢弃连接（TCP 无 hunt）

        if (!recvAll(clientFd, pdu.data(), pduLen)) break;

        std::vector<uint8_t> respPdu;
        if (m_handler) {
            respPdu = m_handler(std::vector<uint8_t>(pdu.data(), pdu.data() + pduLen));
        } else {
            respPdu = dispatchRequest(m_regs, pdu.data(), pduLen);
        }
        if (respPdu.empty()) break;                 // 非法请求 → 丢弃连接

        core::MbapHeader resp;
        resp.transactionId = hdr.transactionId;     // 透传（LLD-SIM §3.1）
        resp.protocolId    = 0;
        resp.length        = static_cast<uint16_t>(respPdu.size() + 1);    // unitId + PDU
        resp.unitId        = hdr.unitId;

        core::emit_mbap(mbap.data(), resp);
        if (!sendAll(clientFd, mbap.data(), mbap.size())) break;
        if (!sendAll(clientFd, respPdu.data(), respPdu.size())) break;
    }

    {
        std::lock_guard<std::mutex> lock(m_clientsMtx);
        m_clients.erase(std::remove(m_clients.begin(), m_clients.end(), clientFd), m_clients.end());
    }
    closeFd(clientFd);
}

}  // namespace ens::sim