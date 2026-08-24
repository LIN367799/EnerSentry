// tests/integration/test_modbus_tcp_loopback.cpp
// B3 Tier 3 集成：进程内起 ModbusTcpServer(127.0.0.1:0) → 原生 TCP 客户端连入，
// 经 core::mbap 组 FC03/06 请求 → 校验响应（transactionId 透传 + 大端 + 写落盘）。
// 客户端/服务端同进程跑真实 socket 通路，无需外部工具（ENS-DEV-GUIDE §2B B3 T3 降级自动化）。

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "core/mbap.h"
#include "sim/modbus_tcp_server.h"

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

using ens::sim::ModbusTcpServer;

namespace {

#ifdef _WIN32
using SockLen = int;
#else
using SockLen = socklen_t;
#endif

class TestClient {
public:
    explicit TestClient(uint16_t port) {
#ifdef _WIN32
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
#endif
        m_fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        REQUIRE(m_fd >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    }

    ~TestClient() {
#ifdef _WIN32
        if (m_fd >= 0) closesocket(m_fd);
#else
        if (m_fd >= 0) ::close(m_fd);
#endif
    }

    bool sendFrame(uint16_t tid, uint8_t fc, const std::vector<uint8_t>& pduTail) {
        std::vector<uint8_t> frame;
        ens::core::MbapHeader h;
        h.transactionId = tid;
        h.length = static_cast<uint16_t>(pduTail.size() + 1 + 1);   // fc + tail + unitId
        h.unitId = 1;
        std::array<uint8_t, 7> mbap{};
        ens::core::emit_mbap(mbap.data(), h);
        frame.insert(frame.end(), mbap.begin(), mbap.end());
        frame.push_back(fc);
        frame.insert(frame.end(), pduTail.begin(), pduTail.end());
        return sendAll(frame.data(), frame.size());
    }

    bool recvFrame(std::vector<uint8_t>& out, uint16_t* outTid = nullptr) {
        std::array<uint8_t, 7> mbap{};
        if (!recvAll(mbap.data(), mbap.size())) return false;
        ens::core::MbapHeader h;
        if (!ens::core::parse_mbap(mbap.data(), mbap.size(), h)) return false;
        if (outTid) *outTid = h.transactionId;
        out.assign(h.length - 1, 0);                                // 去掉 unitId
        return recvAll(out.data(), out.size());
    }

    // 非阻塞：接收缓冲是否还有待读数据（验证"响应恰好一帧"）
    bool hasPendingData() {
#ifdef _WIN32
        u_long n = 0;
        return ioctlsocket(m_fd, FIONREAD, &n) == 0 && n > 0;
#else
        int n = 0;
        return ioctl(m_fd, FIONREAD, &n) == 0 && n > 0;
#endif
    }

private:
    bool recvAll(uint8_t* buf, size_t n) {
        size_t got = 0;
        while (got < n) {
#ifdef _WIN32
            const int r = ::recv(m_fd, reinterpret_cast<char*>(buf + got), static_cast<int>(n - got), 0);
#else
            const ssize_t r = ::recv(m_fd, buf + got, n - got, 0);
#endif
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        return true;
    }

    bool sendAll(const uint8_t* buf, size_t n) {
        size_t sent = 0;
        while (sent < n) {
#ifdef _WIN32
            const int w = ::send(m_fd, reinterpret_cast<const char*>(buf + sent), static_cast<int>(n - sent), 0);
#else
            const ssize_t w = ::send(m_fd, buf + sent, n - sent, 0);
#endif
            if (w <= 0) return false;
            sent += static_cast<size_t>(w);
        }
        return true;
    }

    int m_fd = -1;
};

}  // namespace

TEST_CASE("tcp_server: open binds OS-assigned port and reports actualPort", "[sim][tcp]") {
    ModbusTcpServer server("127.0.0.1", 0);
    REQUIRE(server.open());
    CHECK(server.isOpen());
    CHECK(server.actualPort() > 0);
    server.close();
    CHECK_FALSE(server.isOpen());
}

TEST_CASE("tcp_server: FC03 read round-trip with transactionId passthrough", "[sim][tcp]") {
    ModbusTcpServer server("127.0.0.1", 0);
    REQUIRE(server.open());
    server.regs().set(0, 0x1234);
    server.regs().set(1, 0xABCD);

    TestClient client(server.actualPort());
    REQUIRE(client.sendFrame(0x7B2A, 0x03, {0x00, 0x00, 0x00, 0x02}));   // 读 addr=0 qty=2

    std::vector<uint8_t> resp;
    uint16_t tid = 0;
    REQUIRE(client.recvFrame(resp, &tid));
    CHECK(tid == 0x7B2A);                                               // transactionId 透传

    REQUIRE(resp.size() == 6);                                            // fc + byteCount + 2×2B
    CHECK(resp[0] == 0x03);                                             // fc
    CHECK(resp[1] == 4);                                                // byteCount = 2 regs × 2B
    CHECK(resp[2] == 0x12);                                             // 0x1234 大端
    CHECK(resp[3] == 0x34);
    CHECK(resp[4] == 0xAB);                                             // 0xABCD
    CHECK(resp[5] == 0xCD);
    CHECK_FALSE(client.hasPendingData());                               // 响应恰好一帧（MBAP length 精确切分）

    server.close();
}

TEST_CASE("tcp_server: FC06 write persists and is readable back", "[sim][tcp]") {
    ModbusTcpServer server("127.0.0.1", 0);
    REQUIRE(server.open());

    TestClient client(server.actualPort());
    REQUIRE(client.sendFrame(0x0001, 0x06, {0x00, 0x05, 0xCA, 0xFE}));  // 写 addr=5 val=0xCAFE

    std::vector<uint8_t> echo;
    REQUIRE(client.recvFrame(echo));
    REQUIRE(echo.size() == 5);
    CHECK(echo[0] == 0x06);                                             // 写回显
    CHECK(echo[1] == 0x00);
    CHECK(echo[2] == 0x05);
    CHECK(echo[3] == 0xCA);
    CHECK(echo[4] == 0xFE);

    // 读回验证落盘
    REQUIRE(client.sendFrame(0x0002, 0x03, {0x00, 0x05, 0x00, 0x01}));
    std::vector<uint8_t> rd;
    REQUIRE(client.recvFrame(rd));
    REQUIRE(rd.size() == 4);
    CHECK(rd[2] == 0xCA);
    CHECK(rd[3] == 0xFE);

    server.close();
}

TEST_CASE("tcp_server: close is idempotent", "[sim][tcp]") {
    ModbusTcpServer server("127.0.0.1", 0);
    REQUIRE(server.open());
    server.close();
    server.close();                                                     // 二次调用不抛
    CHECK_FALSE(server.isOpen());
}