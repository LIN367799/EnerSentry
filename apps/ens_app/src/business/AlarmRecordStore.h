// src/business/AlarmRecordStore.h —— 告警记录持久化（切片 35，FR-AL-13 数据面）。
//
// 职责：订阅 AlarmEngine 全生命周期信号（triggered / recovered / acknowledged /
// 风暴批），把告警事件即时写入独立告警库 alarm_YYYYMM.db（DBDD §4.4 静态隔离，
// 经 datahub::SQLiteDataAccess 扩展 API，业务层不直接碰 SQLite 连接）。
//
// 设计要点：
//   * 恢复/确认事件仅带 alarmId → 内部 map<id, triggerTime> 路由到记录所在月份库；
//     记录 Recovered（终态）后从 map 移除。
//   * 进程重启后 map 为空，历史 Active 记录的恢复/确认无法回填 —— 引擎状态本随
//     进程清零，DB 停 Active 属一致语义（接受边界）。
//   * 恢复/确认时间戳由本类接收信号时打墙钟（毫秒级误差，可接受）。
//   * 主线程使用（AlarmEngine 同线程 direct connect，无跨线程 marshal）；
//     dataRootDir 为空（--data-dir 缺省）= 落库禁用，no-op 不崩溃。
#pragma once

#include <ens/export.hpp>
#include "AlarmEntities.h"   // AlarmEvent / AlarmLevel

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ens::datahub {
class SQLiteDataAccess;
struct AlarmRecord;
}  // namespace ens::datahub

namespace ens::business {

class AlarmEngine;

class ENS_BUSINESS_API AlarmRecordStore : public QObject {
    Q_OBJECT
public:
    /// @param engine 告警引擎（不拥有；须与 store 同线程）
    /// @param dal    数据访问层（不拥有；dataRootDir 为空 → 本 store 自动禁用）
    explicit AlarmRecordStore(AlarmEngine* engine, datahub::SQLiteDataAccess* dal,
                              QObject* parent = nullptr);
    ~AlarmRecordStore() override;

    AlarmRecordStore(const AlarmRecordStore&) = delete;
    AlarmRecordStore& operator=(const AlarmRecordStore&) = delete;

    /// 落库是否启用（dal 有效且 dataDir 非空）
    bool isEnabled() const noexcept { return m_enabled; }
    /// 落库失败批次计数（诊断）
    uint64_t droppedCount() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

private slots:
    void onAlarmTriggered(const ens::business::AlarmEvent& ev);
    void onAlarmRecovered(uint64_t alarmId);
    void onAlarmAcknowledged(uint64_t alarmId, const QString& user);
    void onStormBatch(int totalCount, int droppedCount,
                      const QVector<ens::business::AlarmEvent>& samples);

private:
    /// 墙钟 Unix ms（恢复/确认时间戳）
    static uint64_t epochNowMs();

    AlarmEngine*                m_engine = nullptr;   // 不拥有
    datahub::SQLiteDataAccess*  m_dal    = nullptr;   // 不拥有
    bool                        m_enabled = false;
    /// alarmId → triggerTime（恢复/确认按此路由月份库；Recovered 终态后移除）
    std::unordered_map<uint64_t, uint64_t> m_triggerByAlarmId;
    std::atomic<uint64_t>       m_dropped{0};
};

}  // namespace ens::business
