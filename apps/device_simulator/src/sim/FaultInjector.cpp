// src/sim/FaultInjector.cpp
// 测试台 ── 故障注入调度器实现（ENS-LLD-SIM §4.3-§4.5 + DevGuide §4B B7）。
//
// 关键设计：
//   * OverrideTable 快照替换 RCU：写路径持 m_sessionsMtx 排他锁后 rebuildTable→赋值 m_tablePtr
//   * 读路径 resolveOverride 持 shared_lock 短时间：拷 m_tablePtr 共享指针 + 遍历 sessions
//   * FaultSession FSM 推进：tickSessions 由 PointGenerator 每 tick 前同步调用
//   * 五类故障 FaultEffect 推导：OverTemp/CellVoltage → value=target + active
//     CommLoss → dropLink=true；CrcError → corruptCrc/corruptByte=true；Timeout → delayMs=corruptMs

#include "sim/FaultInjector.h"

#include <algorithm>
#include <chrono>

namespace ens::sim {

// ═════════════════════════════════════════════════════════════════════════════
// 单调时钟
// ═════════════════════════════════════════════════════════════════════════════

int64_t FaultInjector::nowMonoMs() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ═════════════════════════════════════════════════════════════════════════════
// OverrideTable
// ═════════════════════════════════════════════════════════════════════════════

namespace {
inline FaultEffect entryToEffect(const OverrideTable::Entry& e) noexcept {
    FaultEffect ef{};
    ef.active      = true;
    ef.value       = e.value;
    ef.corruptCrc  = e.corruptCrc;
    ef.corruptByte = e.corruptByte;
    ef.dropLink    = e.dropLink;
    ef.delayMs     = e.delayMs;
    ef.type        = e.type;
    return ef;
}
}  // namespace

FaultEffect OverrideTable::resolve(uint8_t slave, uint16_t reg) const noexcept {
    // 精确匹配 (slave,reg)
    const auto it = m_entries.find(keyOf(slave, reg));
    if (it != m_entries.end()) {
        return entryToEffect(it->second);
    }
    // ALL/SLAVE scope 通配：靠 m_entries 里的特殊 key，由 FaultInjector 写入时按 scope 决策
    // POINT scope 不会写到 ALL_KEY
    const auto ait = m_entries.find(ALL_KEY);
    if (ait != m_entries.end()) {
        return entryToEffect(ait->second);
    }
    return FaultEffect{};
}

void OverrideTable::set(uint32_t key, Entry e) noexcept {
    m_entries[key] = e;
}

void OverrideTable::clearAll() noexcept {
    m_entries.clear();
}

// ═════════════════════════════════════════════════════════════════════════════
// FaultSession
// ═════════════════════════════════════════════════════════════════════════════

FaultSession::FaultSession(FaultHandle h, FaultOverride spec, int64_t startMonoMs) noexcept
    : m_handle(h), m_spec(spec), m_startMonoMs(startMonoMs), m_recoverTarget(spec.recoverValue) {}

void FaultSession::toActive(int64_t nowMonoMs) noexcept {
    m_state = FaultState::ACTIVE;
    m_activeSinceMs = nowMonoMs;
    m_currentValue = m_spec.targetValue;
}

void FaultSession::toRecovering() noexcept {
    m_state = FaultState::RECOVERING;
    m_recoverSinceMs = FaultInjector::nowMonoMs();
    m_recoverTarget = m_spec.recoverValue;   // 切片 15：recover(h, rv) 会先 set 再转移
}

void FaultSession::toAborted() noexcept {
    m_state = FaultState::ABORTED;
    m_currentValue = 0.0f;
}

void FaultSession::toIdle() noexcept {
    m_state = FaultState::IDLE;
    m_currentValue = 0.0f;
}

void FaultSession::tick(int64_t nowMonoMs) noexcept {
    if (m_state == FaultState::ACTIVE) {
        if (m_spec.durationMs > 0) {
            const int64_t elapsed = nowMonoMs - m_activeSinceMs;
            if (elapsed >= m_spec.durationMs) {
                toRecovering();
            }
        }
        // durationMs == 0 表示永久 ACTIVE（外部必须显式 recover/abort）
    } else if (m_state == FaultState::RECOVERING) {
        // 线性回归 currentValue → m_recoverTarget（切片 15：目标可为 spec.recoverValue，
        // drill RECOVER step 的 targetValue 即回归目标；默认 0 与旧行为一致）
        const int64_t dt = nowMonoMs - m_recoverSinceMs;
        if (dt <= 0) return;
        const float delta = m_spec.rampRate * (static_cast<float>(dt) / 1000.0f);
        const float target = m_recoverTarget;
        if (m_currentValue > target) {
            m_currentValue = std::max(target, m_currentValue - delta);
        } else if (m_currentValue < target) {
            m_currentValue = std::min(target, m_currentValue + delta);
        }
        if (std::abs(m_currentValue - target) < kRecoverEpsilon) {
            m_currentValue = target;
            toIdle();
        }
    }
}

bool FaultSession::covers(uint8_t slave, uint16_t reg) const noexcept {
    if (m_state != FaultState::ACTIVE && m_state != FaultState::RECOVERING) {
        return false;
    }
    switch (m_spec.scope) {
        case Scope::POINT:
            return (m_spec.slave == slave) && (m_spec.reg == reg);
        case Scope::SLAVE:
            return (m_spec.slave == slave);
        case Scope::ALL:
            return true;  // B7 简化：无差别覆盖
    }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// FaultInjector
// ═════════════════════════════════════════════════════════════════════════════

FaultInjector::FaultInjector() {
    m_tablePtr = std::make_shared<const OverrideTable>();
}

FaultInjector::~FaultInjector() = default;

FaultEffect FaultInjector::resolveOverride(uint8_t slave, uint16_t reg) const noexcept {
    // RCU 读路径：持 shared_lock 拷 m_tablePtr 共享指针 + 遍历 sessions 拿 currentValue
    std::shared_lock<std::shared_mutex> lk(m_sessionsMtx);
    if (!m_tablePtr) return FaultEffect{};

    FaultEffect ef = m_tablePtr->resolve(slave, reg);

    if (ef.active) {
        for (const auto& s : m_sessions) {
            if (s.covers(slave, reg)) {
                ef.value = s.currentValue();
                break;
            }
        }
    }
    return ef;
}

FaultEffect FaultInjector::linkEffect(uint8_t slave) const noexcept {
    // B8 IO 层链路级故障查询：走 sessions 遍历(不走 m_tablePtr,因为 POINT scope 写的是
    // 精确 key (slave<<16|reg),m_tablePtr 的 ALL_KEY 通配只对 SLAVE/ALL scope 命中)。
    // 匹配规则:scope=POINT 且 slave 匹配;SLAVE 且 slave 匹配;ALL 一律匹配
    // (IO 层不区分 reg,所以 POINT scope 的 IO 查询也只看 slave)。
    // 多 session 同 slave 合并(同 session 的所有 effect 标志全部应用)。
    std::shared_lock<std::shared_mutex> lk(m_sessionsMtx);
    FaultEffect ef{};
    for (const auto& s : m_sessions) {
        const auto st = s.state();
        if (st != FaultState::ACTIVE && st != FaultState::RECOVERING) continue;
        const auto& spec = s.spec();
        bool match = false;
        switch (spec.scope) {
            case Scope::POINT: match = (spec.slave == slave); break;
            case Scope::SLAVE: match = (spec.slave == slave); break;
            case Scope::ALL:   match = true; break;
        }
        if (!match) continue;
        ef.active = true;
        ef.type   = spec.type;
        if (spec.type == FaultType::CommLoss) {
            ef.dropLink = true;
        }
        if (spec.type == FaultType::CrcError) {
            ef.corruptCrc  = true;
            ef.corruptByte = true;
        }
        if (spec.type == FaultType::Timeout) {
            ef.delayMs = spec.corruptMs;
        }
    }
    return ef;
}

FaultHandle FaultInjector::trigger(const FaultRequest& req) noexcept {
    const FaultHandle h = m_nextHandle.fetch_add(1, std::memory_order_relaxed);
    if (h == INVALID_FAULT_HANDLE) {
        return INVALID_FAULT_HANDLE;
    }
    {
        std::unique_lock<std::shared_mutex> lk(m_sessionsMtx);
        m_sessions.emplace_back(h, req.spec, nowMonoMs());
        m_sessions.back().toActive(nowMonoMs());
        rebuildTableNoLock();
    }
    return h;
}

bool FaultInjector::recover(FaultHandle h, float recoverValue) noexcept {
    bool ok = false;
    {
        std::unique_lock<std::shared_mutex> lk(m_sessionsMtx);
        for (auto& s : m_sessions) {
            if (s.handle() == h && s.state() == FaultState::ACTIVE) {
                s.setRecoverTarget(recoverValue);   // 切片 15：回归目标
                s.toRecovering();
                ok = true;
                break;
            }
        }
        if (ok) rebuildTableNoLock();
    }
    return ok;
}

bool FaultInjector::abort(FaultHandle h) noexcept {
    bool ok = false;
    {
        std::unique_lock<std::shared_mutex> lk(m_sessionsMtx);
        for (auto& s : m_sessions) {
            if (s.handle() == h) {
                s.toAborted();
                ok = true;
                break;
            }
        }
        if (ok) rebuildTableNoLock();
    }
    return ok;
}

void FaultInjector::tickSessions(uint32_t dtMs) noexcept {
    (void)dtMs;  // 当前用 monotonic 时钟；dtMs 保留给未来节流/批处理
    const int64_t now = nowMonoMs();
    bool changed = false;
    {
        std::unique_lock<std::shared_mutex> lk(m_sessionsMtx);
        for (auto& s : m_sessions) {
            const FaultState before = s.state();
            s.tick(now);
            if (before != s.state()) changed = true;
        }
        if (changed) rebuildTableNoLock();
    }
}

size_t FaultInjector::sessionCount() const noexcept {
    std::shared_lock<std::shared_mutex> lk(m_sessionsMtx);
    return m_sessions.size();
}

FaultState FaultInjector::sessionState(FaultHandle h) const noexcept {
    std::shared_lock<std::shared_mutex> lk(m_sessionsMtx);
    for (const auto& s : m_sessions) {
        if (s.handle() == h) return s.state();
    }
    return FaultState::IDLE;
}

size_t FaultInjector::tableSize() const noexcept {
    std::shared_lock<std::shared_mutex> lk(m_sessionsMtx);
    return m_tablePtr ? m_tablePtr->size() : 0;
}

void FaultInjector::rebuildTable() noexcept {
    std::unique_lock<std::shared_mutex> lk(m_sessionsMtx);
    rebuildTableNoLock();
}

void FaultInjector::rebuildTableNoLock() noexcept {
    auto next = std::make_shared<OverrideTable>();
    for (const auto& s : m_sessions) {
        const auto& spec = s.spec();
        if (s.state() != FaultState::ACTIVE && s.state() != FaultState::RECOVERING) {
            continue;
        }
        OverrideTable::Entry e{};
        e.value       = s.currentValue();
        e.type        = spec.type;
        e.dropLink    = (spec.type == FaultType::CommLoss);
        e.corruptCrc  = (spec.type == FaultType::CrcError);
        e.corruptByte = (spec.type == FaultType::CrcError);
        e.delayMs     = (spec.type == FaultType::Timeout) ? spec.corruptMs : 0;

        switch (spec.scope) {
            case Scope::POINT:
                next->set(OverrideTable::keyOf(spec.slave, spec.reg), e);
                break;
            case Scope::SLAVE:
                // SLAVE scope 简化为"该 slave 全部 reg 覆盖"——m_entries 用 ALL_KEY 通配，
                // FaultSession::covers 内做 slave 严格匹配
                next->set(OverrideTable::ALL_KEY, e);
                break;
            case Scope::ALL:
                next->set(OverrideTable::ALL_KEY, e);
                break;
        }
    }
    m_tablePtr = std::move(next);
}

}  // namespace ens::sim
