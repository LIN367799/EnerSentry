// src/datahub/IDataAccess.h —— L3 历史数据访问抽象（ENS-LLD-500 §0.4 / LLD-200 §4.3）。
// UI 历史查询（HistoryTrendWidget）仅依赖本抽象注入，严禁 include SQLiteDataAccess.h
// （LLD-500 §0.4 铁律：UI 主线程不接触具体 SQLite 实现）。
// 查询 API 对告警审计 / 报表等后续消费者复用。
#pragma once

#include "DownSampler.h"   // DownSampledSample / HistoryGranularity

#include <cstdint>
#include <vector>

namespace ens::datahub {

class IDataAccess {
public:
    virtual ~IDataAccess() = default;

    /// 查询某测点 [beginMs, endMs) 时间范围的历史聚合（跨月自动路由）。
    /// @param beginMs/endMs Unix 毫秒；beginMs >= endMs 返回空
    /// @param gran 落库粒度（表后缀 _100ms/_1s/_5s/_1m）
    /// @return 按 ts 升序的聚合样本（月库缺失/打开失败时跳过该月）
    virtual std::vector<DownSampledSample> queryRange(uint32_t pointId,
                                                      uint64_t beginMs,
                                                      uint64_t endMs,
                                                      HistoryGranularity gran) = 0;
};

}  // namespace ens::datahub
