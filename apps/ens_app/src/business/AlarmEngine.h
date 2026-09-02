// src/business/AlarmEngine.h
// L4 业务层 —— 告警引擎实现（ENS-LLD-400 §2.4）。
//
// 线程模型（与 LLD §0.2 / HLD §5.4 一致）：
//   * 热路径 evaluate() 在「告警线程」事件循环内串行执行，无锁。
//   * 配置接口（loadRules/reloadRules/suppressPoint/...）支持跨线程调用；非告警线程
//     调用时内部 marshal 到告警线程私有槽执行（§2.4.1）。
//   * 信号 alarmTriggered / blackBoxRequested / ... 由告警线程发出，订阅方经
//     Qt::AutoConnection 自动 QueuedConnection 投递。
//
// Phase 3 切片 10 (4.3.2) 落地，2026-08-30。

#pragma once

#include <ens/export.hpp>
#include "IAlarmEngine.h"

#include <QObject>
#include <QTimer>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ens::business {

/// 单调时钟工具（steady_clock ms），独立于 wall clock（不受 NTP/手动调时影响）
class MonotonicClock {
public:
    static int64_t nowMs() noexcept;
};

class ENS_BUSINESS_API AlarmEngine : public IAlarmEngine {
    Q_OBJECT
public:
    explicit AlarmEngine(QObject* parent = nullptr);
    ~AlarmEngine() override;

    // —— IAlarmEngine 实现 ——
    void loadRules(const std::vector<AlarmRule>& rules) override;
    void reloadRules(const std::vector<AlarmRule>& rules) override;
    void suppressPoint(uint32_t pointId, uint64_t expireTimeEpoch) override;
    void unsuppressPoint(uint32_t pointId) override;
    void setStormConfig(const AlarmStormConfig& cfg) override;
    bool isInStormMode() const override { return m_stormActive.load(std::memory_order_acquire); }

    // —— 测试/诊断钩子 ——
    /// 当前活跃告警数（Active + Confirmed）
    size_t activeAlarmCount() const noexcept;
    /// 当前规则数
    size_t ruleCount() const noexcept { return m_rules.size(); }
    /// 单测点状态查询（const 视图，测试用）
    bool isPointInAlarm(uint32_t pointId) const noexcept;
    /// 给最近一个 Active 告警生成全局唯一 id（自增；生产可换雪花）
    uint64_t nextAlarmId() noexcept { return ++m_alarmIdSeq; }

public slots:
    void onDataUpdated(uint32_t pointId, uint64_t timestampEpoch, float value) override;
    void acknowledgeAlarm(uint64_t alarmId, const QString& user) override;
    void acknowledgeAlarms(const std::vector<uint64_t>& ids, const QString& user) override;

private slots:
    // —— 跨线程 marshal 私有槽：仅在告警线程事件循环内被调用 ——
    void doReloadRules(const std::vector<AlarmRule>& rules);
    void doSuppressPoint(uint32_t pointId, uint64_t expireTimeEpoch);
    void doUnsuppressPoint(uint32_t pointId);
    void doSetStormConfig(const AlarmStormConfig& cfg);

    void onStormFlush();   ///< 200ms 单发 Flush（风暴模式）

signals:
    void alarmTriggered(const AlarmEvent&);
    void alarmRecovered(uint64_t alarmId);
    /// 切片 35：确认事件带操作人（FR-AL-13 审计字段；AlarmRecordStore 落 confirm_user）
    void alarmAcknowledged(uint64_t alarmId, const QString& user);
    /// Critical 触发 → 外部 connect 到 BlackBoxManager::triggerBlackBox
    /// （依赖倒置：业务层不直接持 datahub 引用）
    /// @param level 告警级别（切片 16：黑匣子落盘需区分 Critical/非 Critical 处置）
    void blackBoxRequested(uint32_t pointId, uint64_t alarmTimeEpoch, AlarmLevel level);
    void alarmStormTriggered(int totalCount, int droppedCount,
                             const QVector<AlarmEvent>& samples);
    void alarmStormCleared();

private:
    // —— 判定核心 ——
    void evaluate(uint32_t pointId, uint64_t tsEpoch, float value);
    void raiseAlarm(uint32_t pid, const AlarmRule& r, float value, int64_t nowMono);
    void recoverAlarm(uint32_t pid, int64_t nowMono,
                      AlarmRecoveryReason reason = AlarmRecoveryReason::Normal);
    /// 风暴触发判定：1s 滑动窗口计数 > threshold → true
    bool isStormTriggered(int64_t nowMono);
    void flushStormBatch();

    // —— 规则缓存（unordered_map，O(1) 查表） ——
    std::unordered_map<uint32_t, AlarmRule>      m_rules;
    std::unordered_map<uint32_t, PointAlarmState> m_states;
    std::unordered_set<uint32_t>                 m_suppressedPoints;     ///< 维护期屏蔽

    // —— 风暴抑制（HLD §5.4.1，LLD §2.3.4） ——
    static constexpr int MAX_PENDING_STORM     = 2000;   ///< 待合并队列硬上限（防 OOM）
    static constexpr int ALARM_TIME_RING_SIZE  = 128;    ///< 1s 滑动窗口最大容量
    AlarmStormConfig      m_stormConfig;
    std::vector<int64_t>  m_alarmTimeRing;             ///< 固定容量环形缓冲区（monotonic）
    size_t                m_ringHead  = 0;             ///< 下一次写入位置
    size_t                m_ringCount = 0;             ///< 当前有效元素数
    std::vector<AlarmEvent> m_pendingStorm;            ///< 待合并批次
    std::atomic<int>      m_stormDroppedCount{0};      ///< 溢出丢弃计数（lock-free）
    std::atomic<bool>     m_stormActive{false};
    QTimer*               m_stormFlushTimer = nullptr; ///< 200ms 单发 Flush

    // —— ID 序列（生产可换雪花） ——
    std::atomic<uint64_t> m_alarmIdSeq{0};
};

}  // namespace ens::business