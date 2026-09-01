// src/business/AlarmEngine.cpp
// L4 业务层 —— 告警引擎实现（ENS-LLD-400 §2.3/2.4）。
//
// 算法严格对齐 LLD：
//   §2.3.1 迟滞比较（inAlarmBand 锁存）
//   §2.3.2 On/Off-Delay（monotonic 时间戳，避免海量 QTimer）
//   §2.3.3 同源抑制（suppressWindowMs，默认 60s）
//   §2.3.4 风暴抑制（MAX_PENDING_STORM=2000，200ms 合并批，droppedCount 原子重置）
//   §2.3.5 Critical → emit blackBoxRequested（依赖倒置，外部连 BlackBoxManager）
//
// Phase 3 切片 10 (4.3.2) 落地，2026-08-30。

#include "AlarmEngine.h"

#include <algorithm>
#include <chrono>

#include <QDebug>
#include <QMetaObject>
#include <QThread>

namespace ens::business {

// ─────────────────────────── MonotonicClock ───────────────────────────
int64_t MonotonicClock::nowMs() noexcept {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace {
/// Unix epoch ms（system_clock；供 triggerTime / blackBoxRequested 用，与
/// L1 采样时间戳同一时间基 —— 勿用 steady_clock，否则 extractRange ±30s 失配）
inline uint64_t epochNowMs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}
}  // namespace

// ─────────────────────────── 构造/析构 ───────────────────────────
AlarmEngine::AlarmEngine(QObject* parent) : IAlarmEngine(parent) {
    // 构造期预分配：消除风暴模式下的 malloc/rehash jitter（评审关注点 ②）
    m_pendingStorm.reserve(MAX_PENDING_STORM);
    m_alarmTimeRing.resize(ALARM_TIME_RING_SIZE);
    m_alarmTimeRing.shrink_to_fit();

    m_stormFlushTimer = new QTimer(this);
    m_stormFlushTimer->setSingleShot(false);
    m_stormFlushTimer->setInterval(200);   // 200ms flush（LLD §2.3.4）
    connect(m_stormFlushTimer, &QTimer::timeout, this, &AlarmEngine::onStormFlush);

    // 注册自定义类型（Qt::QueuedConnection marshalling 需要）
    qRegisterMetaType<AlarmEvent>("ens::business::AlarmEvent");
    qRegisterMetaType<QVector<AlarmEvent>>("QVector<ens::business::AlarmEvent>");
}

AlarmEngine::~AlarmEngine() {
    if (m_stormFlushTimer != nullptr) {
        m_stormFlushTimer->stop();
    }
}

// ─────────────────────────── 跨线程 marshal ───────────────────────────
namespace {
/// 检查是否在告警线程内；不在则用 QueuedConnection 投递到告警线程
template<typename Func>
void marshalToOwnerThread(QObject* owner, Func&& f) {
    if (QThread::currentThread() == owner->thread()) {
        std::forward<Func>(f)();          // 同线程：直调
    } else {
        // 拷贝 f 到 lambda，按值捕获 owner + f，QueuedConnection 排队
        QMetaObject::invokeMethod(owner, std::forward<Func>(f), Qt::QueuedConnection);
    }
}
}  // namespace

void AlarmEngine::loadRules(const std::vector<AlarmRule>& rules) {
    // loadRules 用于初始化期；通常已在告警线程。仍然做 marshal 以保安全。
    marshalToOwnerThread(this, [this, rules]() { doReloadRules(rules); });
}

void AlarmEngine::reloadRules(const std::vector<AlarmRule>& rules) {
    marshalToOwnerThread(this, [this, rules]() { doReloadRules(rules); });
}

void AlarmEngine::suppressPoint(uint32_t pointId, uint64_t expireTimeEpoch) {
    marshalToOwnerThread(this, [this, pointId, expireTimeEpoch]() {
        doSuppressPoint(pointId, expireTimeEpoch);
    });
}

void AlarmEngine::unsuppressPoint(uint32_t pointId) {
    marshalToOwnerThread(this, [this, pointId]() { doUnsuppressPoint(pointId); });
}

void AlarmEngine::setStormConfig(const AlarmStormConfig& cfg) {
    marshalToOwnerThread(this, [this, cfg]() { doSetStormConfig(cfg); });
}

// ─────────────────────────── 配置私有槽 ───────────────────────────
void AlarmEngine::doReloadRules(const std::vector<AlarmRule>& rules) {
    Q_ASSERT(QThread::currentThread() == thread());

    // 1) 重建规则缓存
    std::unordered_map<uint32_t, AlarmRule> newRules;
    newRules.reserve(rules.size() * 2);
    for (const auto& r : rules) {
        if (r.enabled) newRules[r.pointId] = r;
    }

    // 2) 旧状态清理与迁移（LLD §2.4.1）
    const int64_t nowMono = MonotonicClock::nowMs();
    for (auto it = m_states.begin(); it != m_states.end(); ) {
        auto rit = newRules.find(it->first);
        if (rit == newRules.end()) {
            // 规则删除：活跃告警强制恢复
            if (it->second.status == AlarmStatus::Active ||
                it->second.status == AlarmStatus::Confirmed) {
                recoverAlarm(it->first, nowMono, AlarmRecoveryReason::RuleRemoved);
            }
            it = m_states.erase(it);
        } else {
            const auto& oldRule = m_rules[it->first];
            // 阈值/级别变化：重置延时确认状态（防旧时间戳误触发）
            if (oldRule.onThreshold != rit->second.onThreshold ||
                oldRule.offThreshold != rit->second.offThreshold ||
                oldRule.level != rit->second.level) {
                it->second.pendingOn    = false;
                it->second.pendingOff   = false;
                it->second.onSinceMono  = 0;
                it->second.offSinceMono = 0;
                it->second.lastRaiseMono = 0;
            }
            ++it;
        }
    }

    // 3) 原子替换
    m_rules = std::move(newRules);
}

void AlarmEngine::doSuppressPoint(uint32_t pointId, uint64_t expireTimeEpoch) {
    Q_ASSERT(QThread::currentThread() == thread());
    const uint64_t nowEpoch = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (expireTimeEpoch <= nowEpoch) {
        m_suppressedPoints.erase(pointId);   // 已过期
        return;
    }
    m_suppressedPoints.insert(pointId);
}

void AlarmEngine::doUnsuppressPoint(uint32_t pointId) {
    m_suppressedPoints.erase(pointId);
}

void AlarmEngine::doSetStormConfig(const AlarmStormConfig& cfg) {
    m_stormConfig = cfg;
    if (m_stormFlushTimer != nullptr) {
        m_stormFlushTimer->setInterval(static_cast<int>(cfg.flushIntervalMs));
    }
}

// ─────────────────────────── 热路径 evaluate ───────────────────────────
void AlarmEngine::onDataUpdated(uint32_t pointId, uint64_t tsEpoch, float value) {
    // 假定此槽已由 Qt::QueuedConnection 投递到告警线程事件循环
    evaluate(pointId, tsEpoch, value);
}

void AlarmEngine::evaluate(uint32_t pointId, uint64_t tsEpoch, float value) {
    // 1) 规则查找
    auto rit = m_rules.find(pointId);
    if (rit == m_rules.end()) return;                  // 无规则：跳过
    const AlarmRule& r = rit->second;

    // 2) 维护期屏蔽（suppressed）
    if (m_suppressedPoints.count(pointId) != 0) return;

    // 3) 状态查找/创建
    auto sit = m_states.find(pointId);
    if (sit == m_states.end()) {
        sit = m_states.emplace(pointId, PointAlarmState{}).first;
    }
    PointAlarmState& st = sit->second;
    st.pointId  = pointId;
    st.lastRaw  = value;

    const int64_t nowMono = MonotonicClock::nowMs();

    // ═══ §2.3.1 迟滞 + §2.3.2 On/Off-Delay ═══
    if (value > r.onThreshold) {
        // §2.3.4 风暴判定入口：每次越界 evaluate 都记录时间戳(滑动窗口计数)
        (void)isStormTriggered(nowMono);
        // 越界：进入候选（如未在带内）
        if (!st.pendingOn && !st.inAlarmBand) {
            st.pendingOn    = true;
            st.onSinceMono  = nowMono;
        }
        // 持续越界 onDelayMs → 正式产生
        if (st.pendingOn && (nowMono - st.onSinceMono) >= static_cast<int64_t>(r.onDelayMs)) {
            st.pendingOn    = false;
            st.inAlarmBand  = true;
            raiseAlarm(pointId, r, value, nowMono);
        }
    } else if (value < r.offThreshold) {
        // 回落：在带内 → 进入恢复候选
        if (st.inAlarmBand && !st.pendingOff) {
            st.pendingOff   = true;
            st.offSinceMono = nowMono;
        }
        // 持续低于 offThreshold offDelayMs → 恢复
        if (st.pendingOff && (nowMono - st.offSinceMono) >= static_cast<int64_t>(r.offDelayMs)) {
            st.pendingOff  = false;
            st.inAlarmBand = false;
            recoverAlarm(pointId, nowMono);
        }
    } else {
        // 中间带：取消 pending，状态保持（迟滞锁存不翻转）
        st.pendingOn  = false;
        st.pendingOff = false;
    }
}

// ─────────────────────────── raiseAlarm / recoverAlarm ───────────────────────────
void AlarmEngine::raiseAlarm(uint32_t pid, const AlarmRule& r, float value, int64_t nowMono) {
    auto& st = m_states[pid];

    // §2.3.3 同源抑制
    if (r.suppressWindowMs > 0 &&
        (nowMono - st.lastRaiseMono) < static_cast<int64_t>(r.suppressWindowMs)) {
        return;                                          // 抑制：丢弃，不落库不推送
    }

    AlarmEvent evt;
    evt.id          = nextAlarmId();
    evt.pointId     = pid;
    evt.level       = r.level;
    evt.status      = AlarmStatus::Active;
    evt.triggerTime = epochNowMs();                      // Unix ms（落库/显示/黑匣子用）
    evt.alarmValue  = value;
    evt.threshold   = r.onThreshold;
    evt.description = "point=" + std::to_string(pid) + " value=" + std::to_string(value)
                    + " threshold=" + std::to_string(r.onThreshold);

    st.activeId      = evt.id;
    st.status        = AlarmStatus::Active;
    st.lastRaiseMono = nowMono;

    // §2.3.4 风暴抑制：超 MAX_PENDING_STORM → 丢 + droppedCount++
    if (m_stormActive.load(std::memory_order_acquire)) {
        if (m_pendingStorm.size() >= static_cast<size_t>(MAX_PENDING_STORM)) {
            m_stormDroppedCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            m_pendingStorm.push_back(evt);
        }
        // 风暴模式下 Critical 仍走实时路径（不合并）
        if (r.level == AlarmLevel::Critical) {
            emit alarmTriggered(evt);
            emit blackBoxRequested(pid, evt.triggerTime, r.level);
        }
        return;
    }

    // 正常路径
    emit alarmTriggered(evt);
    if (r.level == AlarmLevel::Critical) {
        emit blackBoxRequested(pid, evt.triggerTime, r.level);
    }
}

void AlarmEngine::recoverAlarm(uint32_t pid, int64_t nowMono,
                                AlarmRecoveryReason reason) {
    auto sit = m_states.find(pid);
    if (sit == m_states.end()) return;
    auto& st = sit->second;

    if (st.status != AlarmStatus::Active && st.status != AlarmStatus::Confirmed) {
        return;                                          // 本就未激活
    }

    const uint64_t oldId = st.activeId;
    st.status        = AlarmStatus::Recovered;
    st.activeId      = 0;
    st.inAlarmBand   = false;
    st.pendingOn     = false;
    st.pendingOff    = false;

    emit alarmRecovered(oldId);
    (void)reason;                                        // 未来审计用
}

// ─────────────────────────── 风暴抑制 ───────────────────────────
bool AlarmEngine::isStormTriggered(int64_t nowMono) {
    // 1s 滑动窗口：环形缓冲内 now - ts < windowMs 的计数
    const int64_t windowMs = static_cast<int64_t>(m_stormConfig.windowMs);
    if (windowMs <= 0) return false;

    // 先压入当前时刻
    if (m_ringCount < m_alarmTimeRing.size()) {
        m_alarmTimeRing[(m_ringHead + m_ringCount) % m_alarmTimeRing.size()] = nowMono;
        ++m_ringCount;
    } else {
        // 满：覆盖最老
        m_alarmTimeRing[m_ringHead] = nowMono;
        m_ringHead = (m_ringHead + 1) % m_alarmTimeRing.size();
    }

    // 统计窗口内计数
    size_t count = 0;
    for (size_t i = 0; i < m_ringCount; ++i) {
        if (nowMono - m_alarmTimeRing[i] <= windowMs) ++count;
    }
    const bool active = (count > m_stormConfig.threshold);
    m_stormActive.store(active, std::memory_order_release);
    return active;
}

void AlarmEngine::onStormFlush() {
    if (!m_stormActive.load(std::memory_order_acquire)) return;
    if (m_pendingStorm.empty()) return;
    flushStormBatch();
}

void AlarmEngine::flushStormBatch() {
    const int totalCount   = static_cast<int>(m_pendingStorm.size());
    // 原子读取并重置（LLD §2.3.4 设计：按批次重置）
    const int droppedCount = m_stormDroppedCount.exchange(0, std::memory_order_acq_rel);

    QVector<AlarmEvent> samples;
    samples.reserve(totalCount);
    for (const auto& e : m_pendingStorm) samples.push_back(e);
    m_pendingStorm.clear();

    emit alarmStormTriggered(totalCount, droppedCount, samples);

    // 退出风暴判定：窗口内计数回落 → 取消
    const int64_t nowMono = MonotonicClock::nowMs();
    const int64_t windowMs = static_cast<int64_t>(m_stormConfig.windowMs);
    size_t inWindow = 0;
    for (size_t i = 0; i < m_ringCount; ++i) {
        if (nowMono - m_alarmTimeRing[i] <= windowMs) ++inWindow;
    }
    if (inWindow <= m_stormConfig.threshold) {
        m_stormActive.store(false, std::memory_order_release);
        emit alarmStormCleared();
    }
}

// ─────────────────────────── 确认 / 查询 ───────────────────────────
void AlarmEngine::acknowledgeAlarm(uint64_t alarmId, const QString& user) {
    // 查找 alarmId 对应的 pointId（线性扫描 m_states；N=10K 内 O(N) 可接受）
    for (auto& [pid, st] : m_states) {
        if (st.activeId == alarmId && st.status == AlarmStatus::Active) {
            st.status = AlarmStatus::Confirmed;
            emit alarmAcknowledged(alarmId);
            return;
        }
    }
    // 未找到：仍发 acknowledged（幂等）；上层按需处理
    emit alarmAcknowledged(alarmId);
    (void)user;
}

void AlarmEngine::acknowledgeAlarms(const std::vector<uint64_t>& ids, const QString& user) {
    for (uint64_t id : ids) acknowledgeAlarm(id, user);
}

size_t AlarmEngine::activeAlarmCount() const noexcept {
    size_t n = 0;
    for (const auto& [pid, st] : m_states) {
        if (st.status == AlarmStatus::Active || st.status == AlarmStatus::Confirmed) ++n;
    }
    return n;
}

bool AlarmEngine::isPointInAlarm(uint32_t pointId) const noexcept {
    auto it = m_states.find(pointId);
    if (it == m_states.end()) return false;
    return it->second.inAlarmBand;
}

}  // namespace ens::business