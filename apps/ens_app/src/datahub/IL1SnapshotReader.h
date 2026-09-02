// src/datahub/IL1SnapshotReader.h —— L1 内存快照回放读取抽象（切片 38，FR-AL-12）。
// AlarmCenterWidget 回放弹窗经抽象注入读取高频内存样本（告警 ±30s），
// 严禁 include L1SnapshotStore.h（LLD-500 §0.4 铁律：UI 不触碰 datahub 具体类）。
// 语义：数据仅存于 L1 内存（约 1h 滚动窗口）；滚动淘汰后返回 0（黑匣子文件级
// 回放价值低，暂不做 —— 见切片 38 边界说明）。
#pragma once

#include "Sample.h"

#include <cstdint>

namespace ens::datahub {

class IL1SnapshotReader {
public:
    virtual ~IL1SnapshotReader() = default;

    /// 提取 [beginMs, endMs) 内存高频样本（升序；0=该点未注册或区间已滚动淘汰）
    /// @param out 调用方缓冲（建议 1200 以上：±30s @100ms）
    /// @return 实际写入样本数
    virtual size_t replayExtract(uint32_t pointId, uint64_t beginMs, uint64_t endMs,
                                 Sample* out, size_t maxCount) const noexcept = 0;
};

}  // namespace ens::datahub
