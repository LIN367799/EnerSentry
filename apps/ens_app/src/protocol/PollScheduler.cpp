// apps/ens_app/src/protocol/PollScheduler.cpp
#include "PollScheduler.h"

#include <algorithm>
#include <chrono>

namespace ens::protocol {

namespace {
// 静态启动时间锚点 + 测试偏移共同决定 nowMs()。
// 多线程安全:steady_clock::now() 本身是 thread-safe 的;
// m_clockOffsetMs 写由 advanceClock 单线程做(测试入口),读无并发问题。
const auto kBootAnchor = std::chrono::steady_clock::now();
}

PollScheduler::PollScheduler(QObject* parent) : QObject(parent) {}

PollScheduler::~PollScheduler() = default;

int64_t PollScheduler::nowMs() const noexcept {
    using namespace std::chrono;
    const auto since = duration_cast<milliseconds>(steady_clock::now() - kBootAnchor).count();
    return since + m_clockOffsetMs;
}

void PollScheduler::setLinkParams(uint8_t linkId, const LinkParams& p) {
    m_links[linkId].params = p;
}

void PollScheduler::registerSlave(uint8_t slaveId, uint8_t linkId, int originalIntervalMs) {
    SlavePollState s;
    s.linkId            = linkId;
    s.originalIntervalMs = originalIntervalMs;
    s.currentIntervalMs  = originalIntervalMs;
    s.lastProbeTimeMs    = nowMs();
    m_slaveStates[slaveId] = s;
}

void PollScheduler::unregisterSlave(uint8_t slaveId) noexcept {
    m_slaveStates.erase(slaveId);
}

void PollScheduler::enqueue(const PollTask& task) noexcept {
    LinkState& link = m_links[task.linkId];
    if (task.isControlCommand) {
        link.highPriorityQueue.push_front(task);
    } else if (task.priority == PollPriority::Normal) {
        link.normalQueue.push_back(task);
    } else {
        link.lowPriorityQueue.push_back(task);
    }
}

PollTask PollScheduler::dequeueNext(uint8_t linkId) noexcept {
    auto it = m_links.find(linkId);
    if (it == m_links.end()) return PollTask{};
    LinkState& link = it->second;

    // 半双工总线忙 → 拒下一帧(返回空任务)
    if (link.params.isHalfDuplex && link.busy) {
        return PollTask{};
    }

    const int limit   = link.params.maxConsecutivePreempt;
    const int sboLmit = link.params.maxConsecutiveSboBurst;

    // 1. SBO 风暴防护
    if (!link.highPriorityQueue.empty()) {
        if (link.consecutiveSboCount >= sboLmit && !link.normalQueue.empty()) {
            link.consecutiveSboCount   = 0;
            link.consecutivePreemptCount = 0;
            auto t = link.normalQueue.front(); link.normalQueue.pop_front();
            link.busy = true;   // 半双工占用总线
            return t;
        }
        link.consecutiveSboCount++;
        link.consecutivePreemptCount++;
        auto t = link.highPriorityQueue.front(); link.highPriorityQueue.pop_front();
        link.busy = true;
        return t;
    }

    // 2. LOW 饥饿保护
    if (link.consecutivePreemptCount >= limit && !link.lowPriorityQueue.empty()) {
        link.consecutivePreemptCount = 0;
        auto t = link.lowPriorityQueue.front(); link.lowPriorityQueue.pop_front();
        link.busy = true;
        return t;
    }

    // 3. NORMAL
    if (!link.normalQueue.empty()) {
        link.consecutivePreemptCount++;
        auto t = link.normalQueue.front(); link.normalQueue.pop_front();
        link.busy = true;
        return t;
    }

    // 4. LOW
    if (!link.lowPriorityQueue.empty()) {
        link.consecutivePreemptCount = 0;
        auto t = link.lowPriorityQueue.front(); link.lowPriorityQueue.pop_front();
        link.busy = true;
        return t;
    }

    return PollTask{};
}

void PollScheduler::onLinkFree(uint8_t linkId) noexcept {
    auto it = m_links.find(linkId);
    if (it != m_links.end()) it->second.busy = false;
}

void PollScheduler::onResponseReceived(uint8_t slaveId, bool success) noexcept {
    auto it = m_slaveStates.find(slaveId);
    if (it == m_slaveStates.end()) return;
    SlavePollState& s = it->second;

    if (success) {
        m_successCount++;
        s.consecutiveFailures = 0;
        s.consecutiveSuccesses++;
        s.lastResponseTimeMs = nowMs();
        if (s.health != SlaveHealth::HEALTHY) {
            s.health = SlaveHealth::HEALTHY;
            s.currentIntervalMs = s.originalIntervalMs;
            emit slaveRecovered(slaveId);
        }
    } else {
        m_timeoutCount++;
        s.consecutiveSuccesses = 0;
        s.consecutiveFailures++;
        if (s.consecutiveFailures >= 3 && s.consecutiveFailures < 8) {
            if (s.health == SlaveHealth::HEALTHY) {
                s.health = SlaveHealth::DEGRADED;
                s.currentIntervalMs = s.originalIntervalMs * 3;   // 降级 ×3
                m_degradedCount++;
                emit slaveDegraded(slaveId, s.consecutiveFailures);
            }
        } else if (s.consecutiveFailures >= 8) {
            const bool wasNotIsolated = (s.health != SlaveHealth::ISOLATED);
            s.health = SlaveHealth::ISOLATED;
            s.currentIntervalMs = 30000;                          // ← DoD 30s
            s.lastProbeTimeMs = nowMs();
            if (wasNotIsolated) {
                m_isolatedCount++;
                emit slaveIsolated(slaveId, s.consecutiveFailures);
            }
        }
    }
}

void PollScheduler::enterProbingIfDue(uint8_t slaveId) noexcept {
    auto it = m_slaveStates.find(slaveId);
    if (it == m_slaveStates.end()) return;
    SlavePollState& s = it->second;
    if (s.health == SlaveHealth::ISOLATED &&
        nowMs() - s.lastProbeTimeMs >= 30000) {
        s.health = SlaveHealth::PROBING;
        s.lastProbeTimeMs = nowMs();
        emit slaveProbing(slaveId);
    }
}

SlaveHealth PollScheduler::healthOf(uint8_t slaveId) const noexcept {
    auto it = m_slaveStates.find(slaveId);
    return (it != m_slaveStates.end()) ? it->second.health : SlaveHealth::HEALTHY;
}

int64_t PollScheduler::getNextPollDelayMs(uint8_t slaveId) const noexcept {
    auto it = m_slaveStates.find(slaveId);
    if (it == m_slaveStates.end()) return 0;
    const SlavePollState& s = it->second;
    switch (s.health) {
        case SlaveHealth::ISOLATED: {
            const int64_t since = nowMs() - s.lastProbeTimeMs;
            return std::max<int64_t>(0, 30000 - since);
        }
        case SlaveHealth::PROBING:
            return 0;       // 立即下发
        case SlaveHealth::DEGRADED:
        case SlaveHealth::HEALTHY:
        default:
            return s.currentIntervalMs;
    }
}

bool PollScheduler::isLinkBusy(uint8_t linkId) const noexcept {
    auto it = m_links.find(linkId);
    return (it != m_links.end()) && it->second.busy;
}

void PollScheduler::recomputeCurrentInterval(SlavePollState& s) noexcept {
    // 健康时按原始周期；DEGRADED 时 ×3；ISOLATED 时 30s（由 onResponseReceived 控制）
    if (s.health == SlaveHealth::HEALTHY) {
        s.currentIntervalMs = s.originalIntervalMs;
    } else if (s.health == SlaveHealth::DEGRADED) {
        s.currentIntervalMs = s.originalIntervalMs * 3;
    } else {
        s.currentIntervalMs = 30000;
    }
}

}  // namespace ens::protocol