// src/datahub/DownSampler.h
// L3 数据中枢 ── 降采样聚合引擎（纯算法，无 SQLite 依赖）（ENS-LLD-200 §5.1 / DBDD §4.2）。
//
// 关键设计：
//   * 窗口单向推进:窗口起点对齐到 gran 边界(ts / winMs * winMs)
//   * 桶聚合:Min/Max/Avg/Sum/Count/First/Last(LLD §5.1 草案;First/Last 为 LTO 兼容预留)
//   * rollUp(nowMs=0) 默认系统时间;测试可显式传 nowMs 控制窗口闭合时机
//   * 多测点独立桶表:QHash<pointId, QHash<windowStart, Bucket>>

#pragma once

#include "Sample.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include <QHash>

namespace ens::datahub {

/// 落库粒度（LLD-200 §4.1.1/§5.1）
enum class HistoryGranularity : uint8_t {
    Gran100ms = 0,
    Gran1s    = 1,
    Gran5s    = 2,
    Gran1m    = 3,
};

/// 桶内聚合中间态
struct Bucket {
    uint64_t windowStart = 0;                  // 对齐到窗口起点的 ts
    float    maxV        = -std::numeric_limits<float>::infinity();
    float    minV        =  std::numeric_limits<float>::infinity();
    float    sumV        = 0.0f;
    float    firstV      = 0.0f;               // LTO 兼容预留
    float    lastV       = 0.0f;               // LTO 兼容预留
    uint32_t count       = 0;
    bool     hasFirst    = false;
};

/// 已闭合窗口的可落库结果
struct DownSampledSample {
    uint32_t pointId       = 0;
    uint64_t timestamp     = 0;                // = windowStart
    float    maxValue      = 0.0f;
    float    minValue      = 0.0f;
    float    avgValue      = 0.0f;             // sumV / count(count==0 时 0)
    uint16_t sampleCount   = 0;                // 桶内原始采样数
};

/// 降采样聚合引擎(单实例可承载多测点)
class DownSampler {
public:
    DownSampler() = default;
    ~DownSampler() = default;

    DownSampler(const DownSampler&) = delete;
    DownSampler& operator=(const DownSampler&) = delete;
    DownSampler(DownSampler&&) = delete;
    DownSampler& operator=(DownSampler&&) = delete;

    /// 单测点喂入原始 Sample,内部按粒度分桶
    /// @note 同一 (pointId, gran) 调用链需保证 ts 单调递增(窗口单向推进前提)
    void feed(uint32_t pointId, const Sample& s, HistoryGranularity gran);

    /// 滚出已闭合窗口(返回可落库的聚合结果);nowMs=0 时用系统当前时间
    /// @note 窗口闭合条件:windowStart < alignToWindow(nowMs, gran)
    std::vector<DownSampledSample> rollUp(uint32_t pointId,
                                            HistoryGranularity gran,
                                            uint64_t nowMs = 0);

    /// 对齐到窗口边界(ts / winMs * winMs,向下取整)
    static uint64_t alignToWindow(uint64_t ts, HistoryGranularity gran) noexcept;

    /// 粒度 → 窗口毫秒数
    static uint64_t windowMs(HistoryGranularity gran) noexcept;

    /// 诊断:某测点某粒度当前未闭合桶数(测试可验证)
    size_t bucketCount(uint32_t pointId, HistoryGranularity gran) const;

    /// 诊断:全部清空(测试用)
    void clear() noexcept;

private:
    // 嵌套 QHash:outer=pointId,inner=windowStart
    QHash<uint32_t, QHash<uint64_t, Bucket>> m_buckets;
};

}  // namespace ens::datahub
