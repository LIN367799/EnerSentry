// src/datahub/IAlarmAccess.h —— 告警历史查询抽象（切片 36，FR-AL-11）。
// AlarmCenterWidget 仅依赖本抽象注入查询告警历史（经 UiDeps.alarmAccess），
// 严禁 include SQLiteDataAccess.h（LLD-500 §0.4 铁律：UI 主线程不接触具体 SQLite 实现）。
// 返回 datahub::AlarmRecord（含 FR-AL-13 全字段：触发/恢复/确认人/确认时间/值/阈值/描述）。
#pragma once

#include "AlarmRecord.h"

#include <cstdint>
#include <vector>

namespace ens::datahub {

/// 告警历史查询过滤器（FR-AL-11：时间范围 / 级别 / 测点 / 确认状态）
struct AlarmQueryFilter {
    uint64_t beginMs   = 0;    ///< 触发时间下界（Unix ms；0 = 不约束下限）
    uint64_t endMs     = 0;    ///< 触发时间上界（0 = 不约束上限，取当前时刻）
    int      level     = -1;   ///< 级别过滤（-1 = 全部；0/1/2 = Info/Warning/Critical）
    int      status    = -1;   ///< 状态过滤（-1 = 全部；0/1/2 = Active/Confirmed/Recovered）
    uint32_t pointId   = 0;    ///< 测点过滤（0 = 全部测点）
    int      limit     = 0;    ///< 返回条数上限（0 = 不限）
};

class IAlarmAccess {
public:
    virtual ~IAlarmAccess() = default;

    /// 按过滤器查询告警历史（跨月自动路由），结果按 trigger_time 降序（最新在前）。
    /// @param f 过滤器；endMs=0 时按当前时刻
    /// @return 记录列表；月库缺失/打开失败时跳过该月（与 queryRange 同策略）
    virtual std::vector<AlarmRecord> queryAlarms(const AlarmQueryFilter& f) = 0;
};

}  // namespace ens::datahub
