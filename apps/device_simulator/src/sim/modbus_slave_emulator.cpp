// apps/device_simulator/src/sim/modbus_slave_emulator.cpp
// 见 modbus_slave_emulator.h — 多链路 transport 编排 + unitId 路由 dispatch。

#include "modbus_slave_emulator.h"

#include "sim/modbus_tcp_server.h"
#include "sim/point_generator.h"
#include "sim/rtu_slave_port.h"

#include <algorithm>
#include <cstdio>

namespace ens::sim {

ModbusSlaveEmulator::ModbusSlaveEmulator() = default;

ModbusSlaveEmulator::~ModbusSlaveEmulator() { stop(); }

uint16_t ModbusSlaveEmulator::tcpPort() const noexcept {
    for (const auto& t : m_transports) {
        if (auto* tcp = dynamic_cast<ModbusTcpServer*>(t.get())) {
            return tcp->actualPort();
        }
    }
    return 0;
}

std::string ModbusSlaveEmulator::rtuPort() const noexcept {
    for (const auto& t : m_transports) {
        if (auto* rtu = dynamic_cast<RtuSlavePort*>(t.get())) {
            return "COM";   // RtuSlavePort 不暴露 portName → 这里只给标识
        }
    }
    return {};
}

bool ModbusSlaveEmulator::start(const SimConfig& cfg, RegisterBank* bank) noexcept {
    if (m_running) return true;
    if (bank == nullptr) return false;

    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_bank = bank;
        m_tcpEnabled = cfg.tcp.enabled;
        m_rtuEnabled = cfg.rtu.enabled;
        m_slaves.clear();
        m_transports.clear();

        // ── SlaveRuntime 列表 ──
        m_slaves.reserve(cfg.slaves.size());
        for (const auto& spec : cfg.slaves) {
            SlaveRuntime rt;
            rt.slaveId  = spec.slaveId;
            rt.kind     = spec.kind;
            rt.transport = spec.transport;
            rt.regs     = nullptr;   // 每次按 unitId 查 snapshot
            m_slaves.push_back(rt);
        }

        // ── Transport:TCP 监听 ──
        if (cfg.tcp.enabled) {
            auto tcp = std::make_unique<ModbusTcpServer>(cfg.tcp.bindIp, cfg.tcp.port);
            if (!tcp->open()) return false;
            m_transports.push_back(std::move(tcp));
        }

        // ── Transport:RTU 虚拟串口(com0com / socat) ──
        if (cfg.rtu.enabled) {
            auto rtu = std::make_unique<RtuSlavePort>(cfg.rtu.dev, cfg.rtu.baudRate);
            // com0com 未就绪时,RTU open 失败(LLD-SIM §6.2):优雅降级,
            // 不阻塞 TCP 启动 — DoD 第 4 条「关 rtu.enabled 可纯 TCP 回归」
            // 这里也支持「cfg.rtu.enabled=true 但 open 失败 → 整体启动失败」语义,
            // 由调用方决定是否回退到纯 TCP;为体现 DoD 严格语义,这里返回 false。
            if (!rtu->open()) return false;
            m_transports.push_back(std::move(rtu));
        }

        installDispatchHandler();
    }  // unlock

    m_running = true;
    return true;
}

void ModbusSlaveEmulator::stop() noexcept {
    if (!m_running && m_transports.empty()) return;
    std::lock_guard<std::mutex> lock(m_mtx);
    for (auto& t : m_transports) {
        if (t) t->close();
    }
    m_transports.clear();
    m_running = false;
}

void ModbusSlaveEmulator::installDispatchHandler() noexcept {
    // 闭包捕获 this + bank(已上锁注入)
    // ModbusTcpServer 调 handler 时传 hdr.unitId (MBAP Unit ID)
    // RtuSlavePort    调 handler 时传 p[0]       (RTU 首字节 = slaveAddress)
    // 两者均经 dispatchBySlaveId 路由到对应 SlaveRegset snapshot
    auto cb = [this](uint8_t unitId, const std::vector<uint8_t>& pdu)
              -> std::vector<uint8_t> {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_bank == nullptr) return {};
        auto r = dispatchBySlaveId(unitId, *m_bank, m_slaves, pdu.data(), pdu.size());
        if (!r.has_value()) {
            std::vector<uint8_t> ex{static_cast<uint8_t>(0x80 | (pdu.empty() ? 0xFF : pdu[0])), 0x01};
            return ex;
        }
        return r->bytes;
    };

    for (auto& t : m_transports) {
        if (auto* tcp = dynamic_cast<ModbusTcpServer*>(t.get())) {
            tcp->setRequestHandler(cb);
        } else if (auto* rtu = dynamic_cast<RtuSlavePort*>(t.get())) {
            rtu->setRequestHandler(cb);
        }
        // B8:透传 FaultInjector 到 transport,IO 线程在 invokeHandler 完成后
        // 查 linkEffect(slave) 决定 dropLink / delayMs / corruptCrc / corruptByte
        t->setFaultInjector(m_fi);
    }
}

void ModbusSlaveEmulator::tickOnce(PointGenerator& gen) noexcept {
    std::lock_guard<std::mutex> lock(m_mtx);
    gen.generateTick();   // 调 m_bank->publish 各 slave snapshot
}

size_t ModbusSlaveEmulator::activeTransportCount() const noexcept {
    std::lock_guard<std::mutex> lock(m_mtx);
    size_t n = 0;
    for (const auto& t : m_transports) {
        if (t && t->isOpen()) ++n;
    }
    return n;
}

const SlaveRuntime* ModbusSlaveEmulator::findSlave(uint8_t slaveId) const noexcept {
    for (const auto& s : m_slaves) {
        if (s.slaveId == slaveId) return &s;
    }
    return nullptr;
}

}  // namespace ens::sim