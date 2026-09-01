// src/sim/FaultInjector.h
// 测试台 ── 故障注入调度器（ENS-LLD-SIM §4.3/§4.4 + §3.6 + DevGuide §4B B7）。
//
// 职责（LLD-SIM §2.2.3）：
//   * 维护一组 FaultSession（每个 session 独立 FSM：IDLE→ACTIVE→RECOVERING/ABORTED→IDLE）
//   * resolveOverride(slave, reg) 被 PointGenerator 每 tick 调用，返回该寄存器当前应
//     施加的 FaultEffect（覆盖值/破坏标志/延迟）。无锁读（RCU 快照替换模式）
//   * trigger(req) / recover(h) / abort(h) 操控 FSM；tickSessions(dtMs) 由 PointGenerator
//     每 tick 前驱动 FSM 转移 + 恢复期斜率回归
//
// 关键设计决策（评审记录 2026-08-30）：
//   * R1 OverrideTable 用 std::shared_ptr<const> 原子替换（与 L1SnapshotStore 一致）
//   * R2 publish 一次（带 override），不双 publish baseline
//   * R3 PointGenerator 持 FaultInjector*（不拥有），由 SimulatorEngine 注入（B7 暂未
//     实现 SimulatorEngine 注入者，简化为 PointGenerator::attachFi 直传）
//   * R4 tickSessions 由 PointGenerator 每 tick 调（不开新线程，与 DataTick 单线程架构一致）
//   * R5 FaultHandle = uint32_t 自增 ID
//   * R6 RECOVERING 阶段按 rampRate × elapsedMs 线性插值回归 baseline
//   * R7 不加 storm 硬上限（典型并发 < 10）
//   * R8 Scope 实现：POINT 严格匹配 (slave,reg)；SLAVE 匹配该 slave 全部 reg；
//     ALL 匹配所有 (slave,reg) （B7 简化为"无差别覆盖"——ALL 留给未来按 FaultType
//     过滤；当前测试用例只用 POINT）

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace ens::sim {

// ─────────────────────────────────────────────────────────────────────────────
// 枚举与结构体（LLD-SIM §4.3）
// ─────────────────────────────────────────────────────────────────────────────

enum class FaultType  : uint8_t {
    OverTemp    = 0,
    CellVoltage = 1,
    CommLoss    = 2,
    CrcError    = 3,
    Timeout     = 4,
};

enum class Scope : uint8_t {
    ALL   = 0,
    SLAVE = 1,
    POINT = 2,
};

enum class FaultState : uint8_t {
    IDLE       = 0,
    ACTIVE     = 1,
    RECOVERING = 2,
    ABORTED    = 3,
};

struct FaultOverride {
    FaultType type      = FaultType::OverTemp;
    Scope     scope     = Scope::POINT;
    uint8_t   slave     = 0;
    uint16_t  reg       = 0;
    float     targetValue = 0.0f;   ///< 越限目标值（℃/V）
    float     rampRate    = 0.0f;   ///< ℃/s 或 V/s（ACTIVE 期上升 + RECOVERING 期回归）
    float     recoverValue = 0.0f;  ///< 切片 15：RECOVERING 回归目标（0=归 0,与旧行为一致；
                                    ///< drill RECOVER step 的 targetValue 即回归目标,如 overheat→35℃）
    int32_t   corruptMs   = 0;      ///< CrcError/Timeout 用：响应延迟/破坏毫秒
    int32_t   durationMs  = 0;      ///< ACTIVE 持续时长；0 = 永久 ACTIVE
};

struct FaultEffect {
    bool      active      = false;
    float     value       = 0.0f;
    bool      corruptCrc  = false;   ///< RTU: 坏 CRC（B8 IO 层消费）
    bool      corruptByte = false;   ///< TCP: 破坏 PDU 字节（B8 消费）
    bool      dropLink    = false;   ///< 不响应 / 关连接（B8 消费）
    int32_t   delayMs     = 0;       ///< 响应延迟（B8 消费）
    FaultType type        = FaultType::OverTemp;
};

struct FaultRequest {
    FaultOverride spec{};
};

using FaultHandle = uint32_t;
constexpr FaultHandle INVALID_FAULT_HANDLE = 0;

// ─────────────────────────────────────────────────────────────────────────────
// OverrideTable ── RCU 快照（FaultInjector 内部使用）
// ─────────────────────────────────────────────────────────────────────────────

class OverrideTable {
public:
    struct Entry {
        float     value;
        FaultType type;
        bool      dropLink;
        bool      corruptCrc;
        bool      corruptByte;
        int32_t   delayMs;
    };

    /// resolve: 给定 (slave, reg) 返回当前生效 Effect。
    /// 匹配规则：先查 (slave<<16 | reg) 精确匹配；再查 ALL 通配。
    /// 返回 active=false 表示无覆盖。
    FaultEffect resolve(uint8_t slave, uint16_t reg) const noexcept;

    void set(uint32_t key, Entry e) noexcept;
    void clearAll() noexcept;
    bool empty() const noexcept { return m_entries.empty(); }
    size_t size() const noexcept { return m_entries.size(); }

    static uint32_t keyOf(uint8_t slave, uint16_t reg) noexcept {
        return (uint32_t{slave} << 16) | uint32_t{reg};
    }
    static constexpr uint32_t ALL_KEY = 0xFFFFFFFFu;

private:
    std::unordered_map<uint32_t, Entry> m_entries;
};

// ─────────────────────────────────────────────────────────────────────────────
// FaultSession ── 单个故障会话的 FSM（FaultInjector 内部使用）
// ─────────────────────────────────────────────────────────────────────────────

class FaultSession {
public:
    FaultSession() = default;
    FaultSession(FaultHandle h, FaultOverride spec, int64_t startMonoMs) noexcept;

    /// 状态机推进
    void tick(int64_t nowMonoMs) noexcept;

    /// 显式转移
    void toActive(int64_t nowMonoMs) noexcept;
    void toRecovering() noexcept;
    void toAborted() noexcept;
    void toIdle() noexcept;

    /// 切片 15：设置 RECOVERING 回归目标（recover(h, recoverValue) 用）
    void setRecoverTarget(float v) noexcept { m_recoverTarget = v; }

    /// 是否覆盖 (slave, reg)
    bool covers(uint8_t slave, uint16_t reg) const noexcept;

    /// 当前值（RECOVERING 期 = baseline - rampValue，ACTIVE 期 = targetValue）
    float currentValue() const noexcept { return m_currentValue; }
    FaultState state() const noexcept { return m_state; }
    FaultHandle handle() const noexcept { return m_handle; }
    const FaultOverride& spec() const noexcept { return m_spec; }

    static constexpr float kRecoverEpsilon = 1e-3f;  ///< RECOVERING 回归到位阈值

private:
    FaultHandle  m_handle    = INVALID_FAULT_HANDLE;
    FaultOverride m_spec{};
    FaultState   m_state     = FaultState::IDLE;
    int64_t      m_startMonoMs  = 0;     ///< 触发时刻
    int64_t      m_activeSinceMs = 0;    ///< 进入 ACTIVE 时刻
    int64_t      m_recoverSinceMs = 0;   ///< 进入 RECOVERING 时刻
    float        m_currentValue = 0.0f;  ///< ACTIVE 期 = spec.targetValue；RECOVERING 期渐变
    float        m_recoverTarget = 0.0f; ///< 切片 15：RECOVERING 回归目标（spec.recoverValue）
};

// ─────────────────────────────────────────────────────────────────────────────
// FaultInjector ── 公开 API
// ─────────────────────────────────────────────────────────────────────────────

class FaultInjector {
public:
    FaultInjector();
    ~FaultInjector();

    FaultInjector(const FaultInjector&) = delete;
    FaultInjector& operator=(const FaultInjector&) = delete;

    /// 热路径：PointGenerator 每 tick 每 (slave,reg) 调用一次。无锁读。
    FaultEffect resolveOverride(uint8_t slave, uint16_t reg) const noexcept;

    /// IO 层专用（B8）：查 SLAVE/ALL/POINT scope 的链路级故障效果（dropLink / delayMs / corruptCrc /
    /// corruptByte）。约定忽略 reg（IO 层不区分 reg）。实现走 sessions 遍历（不走 m_tablePtr,
    /// 因为 POINT scope 写的是精确 key,ALL_KEY 通配只对 SLAVE/ALL 命中）。
    /// IO 线程在 invokeHandler 完成后调用一次,根据 effect 决定 corrupt / 丢 / 延迟。
    FaultEffect linkEffect(uint8_t slave) const noexcept;

    /// 触发新故障 session。返回 handle；INVALID_FAULT_HANDLE 表示失败（如 ID 已耗尽）
    FaultHandle trigger(const FaultRequest& req) noexcept;

    /// ACTIVE → RECOVERING（自然到期也走这里，外部可主动调用）。
    /// recoverValue = RECOVERING 回归目标（切片 15；默认 0 = 与旧行为一致归零）
    bool recover(FaultHandle h, float recoverValue = 0.0f) noexcept;

    /// 任意状态 → ABORTED
    bool abort(FaultHandle h) noexcept;

    /// 推进所有 session 的 FSM。PointGenerator 每 tick 前调用（同步推进）。
    void tickSessions(uint32_t dtMs) noexcept;

    /// 诊断
    size_t sessionCount() const noexcept;
    FaultState sessionState(FaultHandle h) const noexcept;
    size_t tableSize() const noexcept;

    /// 单调时钟（steady_clock ms）—— 用于 FSM 计时；与 wall clock 解耦
    static int64_t nowMonoMs() noexcept;

private:
    void rebuildTableNoLock() noexcept;
    void rebuildTable() noexcept;
    mutable std::shared_mutex       m_sessionsMtx;
    std::vector<FaultSession>       m_sessions;
    std::atomic<uint32_t>           m_nextHandle{1};

    // RCU 快照：持 sessionsMtx 排他锁时替换 m_tablePtr；读路径持 shared_lock
    // 拷 m_tablePtr 共享指针 + 遍历 sessions 拿 currentValue
    std::shared_ptr<const OverrideTable> m_tablePtr;
};

}  // namespace ens::sim
