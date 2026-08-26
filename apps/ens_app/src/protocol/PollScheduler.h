// apps/ens_app/src/protocol/PollScheduler.h
// L2 轮询调度器 ── ENS-LLD-100 §4.3 / ENS-LLD-202（Phase 2 切片 5 3.1.4）。
//
// 职责：
//   * 多物理链路调度（每条 link 独立 3 个优先级队列 HIGH/NORMAL/LOW）
//   * 半双工 FIFO 串行保护（RS485 — LLD §4.3.5）+ 全双工独立并发（TCP 专线）
//   * 三级熔断状态机 SlaveHealth (HEALTHY/DEGRADED/ISOLATED/PROBING)
//     + LLD §4.3.3 阈值 (3 failures → DEGRADED, 8 failures → ISOLATED, 30s 试探 → PROBING)
//   * 100ms 插队（BMS NORMAL 队列）+ SBO 风暴防护
//   * 信号：slaveDegraded / slaveIsolated / slaveRecovered / slaveProbing (FR-DIAG-04 UI 联动)
//
// 设计约束（V2.x 增量补丁，2026-08-26 实测坑）：
//   * 时钟注入：提供 advanceClock() 公开方法供单测可控推进；生产用 std::chrono::steady_clock
//   * 内部 nowMs() 走 mutable atomic_clock，便于单线程内 mutate 而不影响多线程语义

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <deque>
#include <unordered_map>
#include <vector>

#include <QObject>

namespace ens::protocol {

// ─────────────────────────────────────────────────────────────────────────────
// 优先级（LLD §4.3.2）
// ─────────────────────────────────────────────────────────────────────────────
enum class PollPriority : uint8_t {
    High   = 0,    // SBO 控制写指令,事件触发立即插队
    Normal = 1,    // BMS 100ms 极速包(独立 TCP 专线)
    Low    = 2,    // 1s 辅机/电表轮询(RS485)
};

// ─────────────────────────────────────────────────────────────────────────────
// 单次轮询任务
// ─────────────────────────────────────────────────────────────────────────────
struct PollTask {
    uint8_t      slaveId          = 0;
    uint8_t      linkId           = 0;
    PollPriority priority         = PollPriority::Normal;
    uint32_t     intervalMs       = 1000;       // 该任务期望轮询周期
    bool         isControlCommand = false;      // SBO 写指令
    uint16_t     registerAddr     = 0;          // 预留(pointId → addr 路由在 Phase 3 接 L4)
    uint16_t     registerQty      = 0;
    bool         isValid() const noexcept { return slaveId != 0 || isControlCommand; }

    static PollTask controlWrite(uint8_t sid, uint8_t lid) {
        PollTask t; t.slaveId = sid; t.linkId = lid;
        t.priority = PollPriority::High; t.isControlCommand = true; return t;
    }
    static PollTask normal(uint8_t sid, uint8_t lid, uint32_t intervalMs) {
        PollTask t; t.slaveId = sid; t.linkId = lid;
        t.priority = PollPriority::Normal; t.intervalMs = intervalMs; return t;
    }
    static PollTask low(uint8_t sid, uint8_t lid, uint32_t intervalMs) {
        PollTask t; t.slaveId = sid; t.linkId = lid;
        t.priority = PollPriority::Low; t.intervalMs = intervalMs; return t;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 链路参数（LLD §4.3.2 V1.3/V1.4）
// ─────────────────────────────────────────────────────────────────────────────
struct LinkParams {
    int maxConsecutivePreempt = 5;       // NORMAL 对 LOW 饥饿保护阈值
    int maxConsecutiveSboBurst = 10;     // SBO 控制写连续下发上限(风暴防护)
    int estimatedFrameTimeMs = 30;       // 单帧估计耗时(供自动计算缺省值)
    bool isHalfDuplex = true;            // RS485 半双工串行保护
};

// ─────────────────────────────────────────────────────────────────────────────
// 从站熔断状态（LLD §4.3.3）
// ─────────────────────────────────────────────────────────────────────────────
enum class SlaveHealth : uint8_t {
    HEALTHY  = 0,    // 正常轮询
    DEGRADED = 1,    // 降级 ×3(连续 3~7 次失败)
    ISOLATED = 2,    // 隔离,30s 探测一次(连续 ≥8 次失败)
    PROBING  = 3,    // 隔离后单次试探(30s 满)
};

// ─────────────────────────────────────────────────────────────────────────────
// 从站轮询状态
// ─────────────────────────────────────────────────────────────────────────────
struct SlavePollState {
    int         consecutiveFailures = 0;
    int         consecutiveSuccesses = 0;
    SlaveHealth health = SlaveHealth::HEALTHY;
    int64_t     lastProbeTimeMs = 0;       // ISOLATED/PROBING 转换时间锚
    int64_t     lastResponseTimeMs = 0;
    int         originalIntervalMs = 1000;
    int         currentIntervalMs = 1000;
    uint8_t     linkId = 0;                // 归属物理链路
};

// ─────────────────────────────────────────────────────────────────────────────
// 链路运行时状态
// ─────────────────────────────────────────────────────────────────────────────
struct LinkState {
    std::deque<PollTask> highPriorityQueue;
    std::deque<PollTask> normalQueue;
    std::deque<PollTask> lowPriorityQueue;
    int  consecutivePreemptCount = 0;
    int  consecutiveSboCount = 0;
    bool busy = false;                      // 半双工:在途帧期间拒绝下一帧出队
    LinkParams params;
};

// ─────────────────────────────────────────────────────────────────────────────
// PollScheduler ── 多链路轮询 + 三级熔断
// ─────────────────────────────────────────────────────────────────────────────
class PollScheduler : public QObject {
    Q_OBJECT

public:
    explicit PollScheduler(QObject* parent = nullptr);
    ~PollScheduler() override;

    PollScheduler(const PollScheduler&) = delete;
    PollScheduler& operator=(const PollScheduler&) = delete;

    // ── 链路 / 从站注册 ──
    void setLinkParams(uint8_t linkId, const LinkParams& p);
    void registerSlave(uint8_t slaveId, uint8_t linkId, int originalIntervalMs);
    void unregisterSlave(uint8_t slaveId) noexcept;

    // ── 任务调度（主路径）──
    /// PollTask 入队；HIGH 抢占队首,NORMAL/LOW 队尾
    void enqueue(const PollTask& task) noexcept;

    /// 取出下一任务；半双工总线忙时返空任务
    /// 同时驱动饥饿保护 + SBO 风暴防护 计数
    PollTask dequeueNext(uint8_t linkId) noexcept;

    /// 半双工:在途帧完成后调 onLinkFree 释放总线
    void onLinkFree(uint8_t linkId) noexcept;

    // ── 熔断统计（每收到一次响应调一次）──
    void onResponseReceived(uint8_t slaveId, bool success) noexcept;

    // ── 探测迁移 ──
    /// 单 tick 调用:对处于 ISOLATED 且距 lastProbeTimeMs ≥ 30s 的从站自动迁到 PROBING
    void enterProbingIfDue(uint8_t slaveId) noexcept;

    // ── 状态查询 ──
    SlaveHealth healthOf(uint8_t slaveId) const noexcept;
    int64_t     getNextPollDelayMs(uint8_t slaveId) const noexcept;
    bool        isLinkBusy(uint8_t linkId) const noexcept;
    int         timeoutCount() const noexcept { return m_timeoutCount.load(); }
    int         successCount() const noexcept { return m_successCount.load(); }
    int         degradedCount() const noexcept { return m_degradedCount.load(); }
    int         isolatedCount() const noexcept { return m_isolatedCount.load(); }

    // ── 时钟注入（单测用,生产走 steady_clock）──
    /// 推进虚拟时钟(毫秒)。生产代码应依赖 nowMs() 走 steady_clock。
    void advanceClock(int64_t ms) noexcept { m_clockOffsetMs += ms; }
    int64_t nowMs() const noexcept;

signals:
    void slaveDegraded(uint8_t slaveId, int consecutiveFailures);
    void slaveIsolated(uint8_t slaveId, int consecutiveFailures);
    void slaveRecovered(uint8_t slaveId);
    void slaveProbing(uint8_t slaveId);

private:
    // ── 内部 ──
    void recomputeCurrentInterval(SlavePollState& s) noexcept;

    // ── 状态 ──
    std::unordered_map<uint8_t, LinkState>     m_links;     // linkId → state
    std::unordered_map<uint8_t, SlavePollState> m_slaveStates;  // slaveId → state
    std::atomic<int> m_timeoutCount{0};
    std::atomic<int> m_successCount{0};
    std::atomic<int> m_degradedCount{0};
    std::atomic<int> m_isolatedCount{0};
    int64_t          m_clockOffsetMs = 0;        // 测试用偏移;真实 nowMs() 走 steady_clock
};

}  // namespace ens::protocol