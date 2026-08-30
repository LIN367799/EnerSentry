// sim/register_bank.h —— Track B B2 多从站寄存器镜像 + RCU 快照库（ENS-LLD-SIM §4.2 / §4.4 / §5.2）。
//
// 责任边界：
//   * SlaveRegs —— 每从站 4 向量镜像（holding/input/coils/discretes），按 registerAddr 索引。
//     热路径不需要对单寄存器原子读写：IO 线程持有 shared_ptr<const SlaveRegs> 只读快照，
//     天然无撕裂读；故 SlaveRegs 本身无需 alignas(16)（仅 WireFrame 在编码端对齐）。
//   * RegisterBank —— 多从站持有 unordered_map<slaveId, shared_ptr<const SlaveRegs>> +
//     std::shared_mutex。生成线程高频 publish（极短 unique_lock 写）；IO 线程并发 snapshot
//     （shared_lock 读，多读者并发不互斥）。旧快照在最后一个持有者释放后自动析构。
//
// 不做（Phase 3+ 收口）：
//   * 热加载/切换配置文件 —— 当前构建期固化从站向量大小。
//   * 控制写（SBO）状态机 —— B6/B7 落 FaultInjector 阶段再做。
//
// ⚠ 与 modbus_slave.h::SlaveRegs（B3 最小 64 uint16_t 寄存器版本）的区别：
//   B3 临时版只支持 FC03/04/06 + 64 holding 寄存器，用于 Phase 1 Track B 最小闭环；
//   本头是 ENS-LLD-SIM §4.2 权威多向量化版，支持 FC01/02/05/0F/15/16 全功能码 + coils/discretes，
//   Track B B5 PointGenerator / B6 ModbusSlaveEmulator 落地时切换。

#pragma once

#include <cstddef>
#include <cstdint>

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace ens::sim {

// ─────────────────────────────────────────────────────────────────────────────
// SlaveRegset —— 每从站 4 向量镜像（LLD-SIM §4.2 权威定义；命名 SlaveRegset 避免与
//                 modbus_slave.h::SlaveRegs（B3 最小 64 uint16_t 寄存器版本）冲突）
// ─────────────────────────────────────────────────────────────────────────────
struct SlaveRegset {
    uint8_t               slaveId = 0;
    std::vector<uint16_t> holding;    // [registerAddr]    HoldingRegisters  4x
    std::vector<uint16_t> input;      // [registerAddr]    InputRegisters    3x
    std::vector<uint8_t>  coils;      // [registerAddr/8]  Coil              0x
    std::vector<uint8_t>  discretes;  // [registerAddr/8]  DiscreteInput     1x

    // B8：簇级告警字。PointGenerator 注入 OverTemp/CellVoltage 时置位;IO 编码时
    // 按 base+0x08（BMS 簇级寄存器 0x00..0x0E 中 alarmWord 槽位,SIM-IMP §3.1）写入 holding。
    // bit 定义（B8 简化为本地定义,联调时与主程序 AlarmEngine 对齐）：
    //   bit0 = OverTemp    bit1 = OverVoltage    bit2 = UnderVoltage
    uint16_t alarmWord = 0;

    // ── HoldingRegisters ──
    uint16_t getHolding(uint16_t reg) const noexcept {
        return (reg < holding.size()) ? holding[reg] : uint16_t{0};
    }
    // 越界静默忽略（LLD-SIM §6.2 — 不抛异常不崩）
    void setHolding(uint16_t reg, uint16_t v) noexcept {
        if (reg < holding.size()) holding[reg] = v;
    }

    // ── InputRegisters（read-only,生成线程写,IO 线程读）──
    uint16_t getInput(uint16_t reg) const noexcept {
        return (reg < input.size()) ? input[reg] : uint16_t{0};
    }
    void setInput(uint16_t reg, uint16_t v) noexcept {
        if (reg < input.size()) input[reg] = v;
    }

    // ── Coils（位打包,byte[reg/8] bit[reg%8]）──
    bool getCoil(uint16_t reg) const noexcept {
        const size_t byteIdx = reg / 8;
        if (byteIdx >= coils.size()) return false;
        return (coils[byteIdx] >> (reg % 8)) & 0x01;
    }
    void setCoil(uint16_t reg, bool v) noexcept {
        const size_t byteIdx = reg / 8;
        if (byteIdx >= coils.size()) return;
        const uint8_t mask = static_cast<uint8_t>(1u << (reg % 8));
        if (v) coils[byteIdx] |= mask;
        else   coils[byteIdx] &= static_cast<uint8_t>(~mask);
    }

    // ── DiscreteInputs ──
    bool getDiscrete(uint16_t reg) const noexcept {
        const size_t byteIdx = reg / 8;
        if (byteIdx >= discretes.size()) return false;
        return (discretes[byteIdx] >> (reg % 8)) & 0x01;
    }
    void setDiscrete(uint16_t reg, bool v) noexcept {
        const size_t byteIdx = reg / 8;
        if (byteIdx >= discretes.size()) return;
        const uint8_t mask = static_cast<uint8_t>(1u << (reg % 8));
        if (v) discretes[byteIdx] |= mask;
        else   discretes[byteIdx] &= static_cast<uint8_t>(~mask);
    }

    // ── 构造：从点表 + slave 拓扑定容 ──
    //   holdingSize/inputSize 按 LLD-SIM §4.2 注（从站作用域偏移）决定
    //   coilsSize = ceil(holdingSize / 8) ;discretesSize = ceil(inputSize / 8)
    static SlaveRegset allocate(uint8_t slaveId,
                                size_t holdingSize,
                                size_t inputSize) {
        SlaveRegset r;
        r.slaveId = slaveId;
        try {
            r.holding.assign(holdingSize, 0);
            r.input.assign(inputSize, 0);
            r.coils.assign((holdingSize + 7) / 8, 0);
            r.discretes.assign((inputSize + 7) / 8, 0);
        } catch (...) {
            // 静默吞下 alloc 异常,返回空 SlaveRegset(越界访问都返 0,不会崩)
        }
        return r;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// RegisterBank —— 多从站 RCU 快照库（LLD-SIM §4.4 / §5.2）
// ─────────────────────────────────────────────────────────────────────────────
class RegisterBank {
public:
    RegisterBank() = default;
    RegisterBank(const RegisterBank&) = delete;
    RegisterBank& operator=(const RegisterBank&) = delete;
    RegisterBank(RegisterBank&&) = delete;
    RegisterBank& operator=(RegisterBank&&) = delete;

    /// 无锁读：从 m_banks[slave] 拷贝 shared_ptr（多读者并发不互斥，shared_lock）
    /// 返回 nullptr 表示该 slave 未初始化（占位尚未 publish）。
    std::shared_ptr<const SlaveRegset> snapshot(uint8_t slave) const noexcept;

    /// 极短写锁：替换 m_banks[slave] 的快照；旧快照在最后一个持有者释放后自动析构。
    void publish(uint8_t slave, std::shared_ptr<const SlaveRegset> next) noexcept;

    /// 初始化：首次 publish 一个 slave（持有者 destroy 时通过 shared_ptr 引用计数释放）。
    /// 与 publish 同语义；提供明确语义名称便于调用方表达"首次建仓"。
    void install(uint8_t slave, std::shared_ptr<const SlaveRegset> initial) noexcept;

    /// SBO 控制字读写（LLD-SIM §4.4）—— Phase 3.x FaultInjector 状态机依赖此接口。
    /// 控制字占用 holding[0..kControlRegCount) 区域(各从站私有)，IO 线程独占读写。
    uint16_t readControl(uint8_t slave, uint16_t reg) const noexcept;
    void     writeControl(uint8_t slave, uint16_t reg, uint16_t v) noexcept;

    /// 诊断：当前管理的 slave 数量
    size_t slaveCount() const noexcept;

    /// 诊断：清空所有从站快照（SimulatorEngine::stop 阶段调用）
    void clear() noexcept;

    // ── B6 新增:从外部 dispatch 写入(FC05/06/0F/10 直接落 SlaveRegset,绕过 PointGenerator)──
    // CoW:取现有 snapshot → 修改 → publish 替换;
    // 多线程并发写需调用方自行加锁(本类内部仅 short unique_lock 写)。
    void writeHolding(uint8_t slave, uint16_t addr, uint16_t v) noexcept;
    void writeCoil(uint8_t slave, uint16_t addr, bool v) noexcept;

    static constexpr uint16_t kControlRegCount = 16;     // 每从站控制字前 16 个寄存器

private:
    mutable std::shared_mutex m_rw;
    std::unordered_map<uint8_t, std::shared_ptr<const SlaveRegset>> m_banks;
};

}  // namespace ens::sim