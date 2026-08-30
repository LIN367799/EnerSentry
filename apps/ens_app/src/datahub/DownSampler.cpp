// src/datahub/DownSampler.cpp
// L3 数据中枢 ── 降采样聚合引擎实现（ENS-LLD-200 §5.1）。

#include "DownSampler.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace ens::datahub {

uint64_t DownSampler::windowMs(HistoryGranularity gran) noexcept {
    switch (gran) {
        case HistoryGranularity::Gran100ms: return 100;
        case HistoryGranularity::Gran1s:    return 1000;
        case HistoryGranularity::Gran5s:    return 5000;
        case HistoryGranularity::Gran1m:    return 60000;
    }
    return 1000;   // 兜底:未知粒度按 1s
}

uint64_t DownSampler::alignToWindow(uint64_t ts, HistoryGranularity gran) noexcept {
    const uint64_t w = windowMs(gran);
    return (ts / w) * w;
}

void DownSampler::feed(uint32_t pointId, const Sample& s, HistoryGranularity gran) {
    if (s.timestamp == 0) return;                                // 边界:非法时间戳

    const uint64_t win = alignToWindow(s.timestamp, gran);
    auto& perPoint = m_buckets[pointId];
    auto it = perPoint.find(win);
    Bucket& b = (it == perPoint.end()) ? perPoint[win] : it.value();

    if (b.windowStart == 0) b.windowStart = win;
    if (!b.hasFirst) { b.firstV = s.value; b.hasFirst = true; }
    b.lastV   = s.value;
    b.maxV    = std::max(b.maxV, s.value);
    b.minV    = std::min(b.minV, s.value);
    b.sumV   += s.value;
    b.count++;
}

std::vector<DownSampledSample> DownSampler::rollUp(uint32_t pointId,
                                                   HistoryGranularity gran,
                                                   uint64_t nowMs) {
    std::vector<DownSampledSample> out;
    auto outerIt = m_buckets.find(pointId);
    if (outerIt == m_buckets.end()) return out;

    // nowMs == 0 → 用系统当前时间
    if (nowMs == 0) {
        nowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }
    const uint64_t nowWin = alignToWindow(nowMs, gran);

    auto& perPoint = outerIt.value();
    for (auto bit = perPoint.begin(); bit != perPoint.end(); ) {
        if (bit.key() < nowWin) {                               // 窗口已闭合
            const Bucket& b = bit.value();
            out.push_back({
                pointId,
                b.windowStart,
                b.maxV,
                b.minV,
                b.count ? (b.sumV / static_cast<float>(b.count)) : 0.0f,
                static_cast<uint16_t>(b.count)
            });
            bit = perPoint.erase(bit);
        } else {
            ++bit;
        }
    }
    return out;
}

size_t DownSampler::bucketCount(uint32_t pointId, HistoryGranularity gran) const {
    auto outerIt = m_buckets.find(pointId);
    if (outerIt == m_buckets.end()) return 0;
    return static_cast<size_t>(outerIt.value().size());
}

void DownSampler::clear() noexcept { m_buckets.clear(); }

}  // namespace ens::datahub
