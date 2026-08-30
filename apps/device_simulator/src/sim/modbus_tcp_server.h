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
#include "sim/FaultInjector.h"  // B8

#include <atomic>
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
    // B6:ModbusSlaveEmulator 通过 unitId(MBAP) 路由到不同 SlaveRegset
    void setRequestHandler(RequestHandlerWithUnit cb) noexcept override;
    // B8:注入 FaultInjector;clientLoop 内 invokeHandler 后查 linkEffect
    void setFaultInjector(FaultInjector* fi) noexcept override { m_fi = fi; }

    // 实际监听端口（open 成功后;bind 0 时由 OS 分配,测试用）
    uint16_t actualPort() const noexcept { return m_port; }
    SlaveRegs& regs() noexcept { return m_regs; }

private:
    void acceptLoop() noexcept;                       // accept 循环（主线程）
    void clientLoop(int clientFd) noexcept;           // 每连接一个 IO 线程
    void dropAllClients() noexcept;                   // close() 时断开全部已接受连接
    static void boostThreadPriority() noexcept;       // HIGHEST 尽力而为
    // B6:获取 handler(优先 unit-aware 版,fallback 经典版),返回 PDU 响应字节
    std::vector<uint8_t> invokeHandler(uint8_t unitId,
                                        const std::vector<uint8_t>& reqPdu);

    std::string m_ip;
    uint16_t    m_port = 0;
    int         m_listenFd = -1;
    std::atomic<bool> m_running{false};               // close() 写、accept/client 线程读（防数据竞争）
    std::thread m_acceptThread;
    std::vector<int> m_clients;                       // 活动 client fd（close 时强制断开）
    std::mutex       m_clientsMtx;
    std::mutex       m_handlerMtx;                    // 保护 m_handler（跨线程读写）
    SlaveRegs   m_regs;
    RequestHandler         m_handler;
    RequestHandlerWithUnit m_handlerWithUnit;        // B6:unitId 路由版本(若设置则优先)
    FaultInjector*         m_fi = nullptr;           // B8:非拥有,setFaultInjector 注入
};

}  // namespace ens::sim
