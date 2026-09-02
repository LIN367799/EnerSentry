// src/business/AlarmRecordStore.cpp —— 告警记录持久化实现（切片 35）。
#include "AlarmRecordStore.h"

#include "AlarmEngine.h"
#include "AlarmRecord.h"
#include "SQLiteDataAccess.h"

#include <QDateTime>
#include <QVector>

namespace ens::business {

namespace {

/// 引擎风暴批样本 → 落库载体（Active 状态，其余字段原样透传）
datahub::AlarmRecord toActiveRecord(const AlarmEvent& ev) {
    datahub::AlarmRecord r;
    r.id           = ev.id;
    r.pointId      = ev.pointId;
    r.level        = static_cast<int>(ev.level);
    r.status       = static_cast<int>(AlarmStatus::Active);
    r.triggerTime  = ev.triggerTime;
    r.alarmValue   = static_cast<double>(ev.alarmValue);
    r.threshold    = static_cast<double>(ev.threshold);
    r.description  = QString::fromStdString(ev.description);
    r.blackboxId   = ev.blackboxId;
    return r;
}

}  // namespace

AlarmRecordStore::AlarmRecordStore(AlarmEngine* engine, datahub::SQLiteDataAccess* dal,
                                   QObject* parent)
    : QObject(parent), m_engine(engine), m_dal(dal) {
    m_enabled = (m_engine != nullptr && m_dal != nullptr && !m_dal->dataRootDir().isEmpty());
    if (!m_enabled) return;   // 无落库目标：不接线，no-op

    // AlarmEngine 与 store 同线程（主线程）→ AutoConnection 为 direct，事件同步落库
    connect(m_engine, &AlarmEngine::alarmTriggered,
            this, &AlarmRecordStore::onAlarmTriggered);
    connect(m_engine, &AlarmEngine::alarmRecovered,
            this, &AlarmRecordStore::onAlarmRecovered);
    connect(m_engine, &AlarmEngine::alarmAcknowledged,
            this, &AlarmRecordStore::onAlarmAcknowledged);
    connect(m_engine, &AlarmEngine::alarmStormTriggered,
            this, &AlarmRecordStore::onStormBatch);
}

AlarmRecordStore::~AlarmRecordStore() = default;

uint64_t AlarmRecordStore::epochNowMs() {
    return static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
}

void AlarmRecordStore::onAlarmTriggered(const AlarmEvent& ev) {
    if (!m_enabled) return;
    m_triggerByAlarmId[ev.id] = ev.triggerTime;
    if (!m_dal->insertAlarmRecords({toActiveRecord(ev)})) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void AlarmRecordStore::onAlarmRecovered(uint64_t alarmId) {
    if (!m_enabled) return;
    const auto it = m_triggerByAlarmId.find(alarmId);
    if (it == m_triggerByAlarmId.end()) return;   // 重启边界：无 trigger 上下文，忽略
    const uint64_t triggerTime = it->second;
    m_triggerByAlarmId.erase(it);                 // Recovered = 终态，释放上下文
    if (!m_dal->setAlarmRecovered(triggerTime, alarmId, epochNowMs())) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void AlarmRecordStore::onAlarmAcknowledged(uint64_t alarmId, const QString& user) {
    if (!m_enabled) return;
    const auto it = m_triggerByAlarmId.find(alarmId);
    if (it == m_triggerByAlarmId.end()) return;   // 已恢复/重启边界：无 Active 记录
    if (!m_dal->setAlarmConfirmed(it->second, alarmId, user, epochNowMs())) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void AlarmRecordStore::onStormBatch(int totalCount, int droppedCount,
                                    const QVector<AlarmEvent>& samples) {
    Q_UNUSED(totalCount);
    if (!m_enabled || samples.isEmpty()) return;
    std::vector<datahub::AlarmRecord> batch;
    batch.reserve(static_cast<size_t>(samples.size()));
    for (const auto& ev : samples) {
        m_triggerByAlarmId[ev.id] = ev.triggerTime;
        batch.push_back(toActiveRecord(ev));
    }
    // 风暴样本同窗口（200ms flush）→ 同月，批量一次事务
    if (!m_dal->insertAlarmRecords(batch)) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    Q_UNUSED(droppedCount);   // 引擎侧丢弃数由引擎诊断；落库侧只计本批失败
}

}  // namespace ens::business
