// sim/modbus_tcp_server.h —— B3 最小 TCP 从站（ENS-DEV-GUIDE §2B B3 / ENS-SIM-IMP §6.1）。
// 原生 socket：bind 127.0.0.1:5020（默认）→ listen → accept 循环；每连接一个 IO 线程。
// 约束（文档契约）：
//   - bind 失败（端口占用）→ port+1 重试 ≤3 次，仍失败 open() 返回 false
//   - MBAP transactionId 原样透传、响应 length 大端回填（复用 core/mbap）
//   - FC03/04/06 由内部 SlaveRegs + dispatchRequest 处理；可经 setRequestHandler 覆盖
//   - close() 幂等
// 纯 C++17 零 Qt；Windows 走 winsock2，POSIX 走 sys/socket。
#pragma once

#include "sim/islave_transport.h"
#include "sim/modbus_slave.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ens::sim {

class ModbusTcpServer : public ISlaveTransport {
public:
    explicit ModbusTcpServer(std::string ip, uint16_t port);
    ~ModbusTcpServer() override;

    bool open() noexcept override;
    void close() noexcept override;
    bool isOpen() const noexcept override { return m_listenFd >= 0; }
    void setRequestHandler(RequestHandler cb) override;

    // 实际监听端口（open 成功后；bind 0 时由 OS 分配，测试用）
    uint16_t actualPort() const noexcept { return m_port; }
    SlaveRegs& regs() noexcept { return m_regs; }

private:
    void acceptLoop() noexcept;                       // accept 循环（主线程）
    void clientLoop(int clientFd) noexcept;           // 每连接一个 IO 线程
    void dropAllClients() noexcept;                   // close() 时断开全部已接受连接
    static void boostThreadPriority() noexcept;       // HIGHEST 尽力而为

    std::string m_ip;
    uint16_t    m_port = 0;
    int         m_listenFd = -1;
    bool        m_running = false;
    std::thread m_acceptThread;
    std::vector<int> m_clients;                       // 活动 client fd（close 时强制断开）
    std::mutex       m_clientsMtx;
    SlaveRegs   m_regs;
    RequestHandler m_handler;
};

}  // namespace ens::sim