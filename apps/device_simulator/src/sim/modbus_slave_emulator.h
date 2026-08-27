// apps/device_simulator/src/sim/modbus_slave_emulator.h
// Modbus 从站模拟器 ── Phase 1 Track B B6（ENS-LLD-SIM §2.2.4 / §4.4 / ENS-SIM-IMP §6）。
//
// 职责:
//   - 持有 vector<unique_ptr<ISlaveTransport>> (TCP + RTU 顺序追加)
//   - 持有 RegisterBank(共用,所有 transport 读同一组 SlaveRegset)
//   - 持有 std::vector<SlaveRuntime> + SimConfig
//   - start():装填 slaves + open transports + 把 unitId-aware handler 装到 transports
//   - tickOnce():PointGenerator.publish 到 bank(由外部 simulator_engine 调用)
//   - stop():优雅关闭
//
// DoD:
//   - 启动后 TCP 5020 + RTU COM 同时监听,读 23 从站(BMS 1~16 + PCS 17~20 + 辅机 21~23)
//     各读几寄存器 → 合理值,均非全 0(物理演化器 PointGenerator 已写入 baseline)
//   - 关 cfg.rtu.enabled → 纯 TCP 回归
//   - 共用同一 RegisterBank(零写副本,B2 RCU 保证 reader 永远看到完整快照)

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "sim/islave_transport.h"
#include "sim/register_bank.h"
#include "sim/sim_config.h"
#include "sim/modbus_dispatch.h"

namespace ens::sim {

class PointGenerator;

class ModbusSlaveEmulator {
public:
    ModbusSlaveEmulator();
    ~ModbusSlaveEmulator();

    ModbusSlaveEmulator(const ModbusSlaveEmulator&) = delete;
    ModbusSlaveEmulator& operator=(const ModbusSlaveEmulator&) = delete;

    /// 启动:从 cfg.tcp/rtu.enabled 决定 open 哪些 transport;
    /// 装 unitId-aware handler 让 PDU 进 dispatchBySlaveId 路由到正确 SlaveRegset;
    /// 同时装载 RegisterBank 中的初始 baseline(由外部 PointGenerator 注入)。
    /// return false:任一必需 transport open 失败。
    bool start(const SimConfig& cfg, RegisterBank* bank) noexcept;

    /// 优雅关闭:close 全部 transport;幂等
    void stop() noexcept;

    /// 单 tick 演化:Phase 2 测试用,Phase 3 由 SimulatorEngine 接管
    void tickOnce(PointGenerator& gen) noexcept;

    /// 状态查询
    bool isRunning() const noexcept { return m_running; }
    uint16_t tcpPort() const noexcept;
    std::string rtuPort() const noexcept;
    bool tcpEnabled() const noexcept { return m_tcpEnabled; }
    bool rtuEnabled() const noexcept { return m_rtuEnabled; }
    size_t activeTransportCount() const noexcept;
    const std::vector<SlaveRuntime>& slaves() const noexcept { return m_slaves; }
    const RegisterBank* bank() const noexcept { return m_bank; }

    // 测试入口
    const SlaveRuntime* findSlave(uint8_t slaveId) const noexcept;

private:
    /// 装 unitId-aware handler 到所有 transport(TCP + RTU)
    void installDispatchHandler() noexcept;

    std::vector<std::unique_ptr<ISlaveTransport>> m_transports;
    std::vector<SlaveRuntime>                    m_slaves;
    RegisterBank*                                 m_bank = nullptr;
    std::atomic<bool>                             m_running{false};
    bool                                          m_tcpEnabled = false;
    bool                                          m_rtuEnabled = false;
    mutable std::mutex                            m_mtx;       // mutable — const methods (activeTransportCount/findSlave) 仍需锁
};

}  // namespace ens::sim