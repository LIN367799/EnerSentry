// src/business/IAlarmEngine.h
// L4 业务层 —— 告警引擎抽象接口（ENS-LLD-400 §2.4）。
//
// 设计：
//   * IAlarmEngine 是纯虚接口（不 Q_OBJECT；为避免 moc 跨 .h 实例化）。AlarmEngine
//     是 Q_OBJECT + signals 实现，订阅方拿到 AlarmEngine* 即可 connect。
//   * 信号源在告警线程发出，订阅方（UI / 持久化）经 Qt::AutoConnection 安全跨线程。

#pragma once

#include <ens/export.hpp>
#include "AlarmEntities.h"

#include <QObject>
#include <QString>
#include <vector>

namespace ens::business {

/// 纯虚接口（不 Q_OBJECT）—— 供 AlarmEngine 派生 + 测试替身 mock
class ENS_BUSINESS_API IAlarmEngine : public QObject {
public:
    explicit IAlarmEngine(QObject* parent = nullptr) : QObject(parent) {}
    ~IAlarmEngine() override = default;

    // —— 配置（支持跨线程安全调用，内部 marshal 到告警线程） ——
    virtual void loadRules(const std::vector<AlarmRule>& rules) = 0;
    virtual void reloadRules(const std::vector<AlarmRule>& rules) = 0;
    virtual void suppressPoint(uint32_t pointId, uint64_t expireTimeEpoch) = 0;
    virtual void unsuppressPoint(uint32_t pointId) = 0;
    virtual void setStormConfig(const AlarmStormConfig& cfg) = 0;
    virtual bool isInStormMode() const = 0;

    /// 由 L2 解析线程经 QueuedConnection 调用（每测点 100ms）
    virtual void onDataUpdated(uint32_t pointId, uint64_t timestampEpoch, float value) = 0;
    virtual void acknowledgeAlarm(uint64_t alarmId, const QString& user) = 0;
    virtual void acknowledgeAlarms(const std::vector<uint64_t>& ids, const QString& user) = 0;
};

}  // namespace ens::business

Q_DECLARE_METATYPE(ens::business::AlarmEvent)