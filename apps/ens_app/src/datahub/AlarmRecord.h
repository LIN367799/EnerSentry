// src/datahub/AlarmRecord.h —— 告警记录持久化载体（切片 35，DBDD §4.4 字段镜像）。
// AlarmEngine（business 层）产生的告警事件经 business::AlarmRecordStore 转成本结构，
// 再经 SQLiteDataAccess 写入独立告警库 alarm_YYYYMM.db（V1.5 静态隔离）。
// 分层约束：datahub 不依赖 business（AlarmEvent 在 business），故落库载体为 datahub 自有纯数据结构。
#pragma once

#include <QString>

#include <cstdint>

namespace ens::datahub {

/// 告警记录（与 DBDD §4.4 alarm_record_YYYYMM 列一一对应）
struct AlarmRecord {
    uint64_t id           = 0;    ///< 告警 ID（AlarmEngine 全局自增）
    uint32_t pointId      = 0;
    int      level        = 0;    ///< 0=Info 1=Warning 2=Critical（与 AlarmLevel 值域一致）
    int      status       = 0;    ///< 0=Active 1=Confirmed 2=Recovered
    uint64_t triggerTime  = 0;    ///< 触发时间（Unix ms）
    uint64_t recoverTime  = 0;    ///< 恢复时间（0=未恢复）
    QString  confirmUser;         ///< 确认人（FR-AL-13）
    uint64_t confirmTime  = 0;    ///< 确认时间（0=未确认）
    double   alarmValue   = 0.0;  ///< 触发时测点值（FR-AL-13）
    double   threshold    = 0.0;  ///< 阈值（FR-AL-13）
    QString  description;         ///< 告警源描述（FR-AL-13）
    uint64_t blackboxId   = 0;    ///< 关联黑匣子 ID（0=无）
};

}  // namespace ens::datahub
