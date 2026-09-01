// src/ui/charts/RenderDownsampler.cpp —— min/max 桶降采样实现。
#include "charts/RenderDownsampler.h"

#include <algorithm>

namespace ens::ui {

QVector<QPointF> RenderDownsampler::minMaxBucketDownSample(const QVector<QPointF>& src,
                                                           int target) {
    if (target < 2 || src.size() <= target) return src;

    // 每桶输出 ≤2 点（min + max），故桶数预算 = target/2，保证总输出 ≤ target
    const int bucketSize = std::max(1, src.size() / std::max(1, target / 2));
    QVector<QPointF> out;
    out.reserve(target);

    for (int i = 0; i < src.size() && out.size() < target; i += bucketSize) {
        const int end = std::min(i + bucketSize, static_cast<int>(src.size()));
        int minIdx = i, maxIdx = i;
        float minY = src[i].y(), maxY = src[i].y();
        for (int j = i + 1; j < end; ++j) {
            if (src[j].y() < minY) { minY = src[j].y(); minIdx = j; }
            if (src[j].y() > maxY) { maxY = src[j].y(); maxIdx = j; }
        }
        out.push_back(src[minIdx]);
        if (maxIdx != minIdx && out.size() < target) {
            out.push_back(src[maxIdx]);
        }
    }

    // 末端补原始最后点：保 x 轴右边界连续（min/max 桶可能略过最后样本）；
    // 仅在仍有预算时补，避免超 target
    if (out.size() < target && out.last().x() != src.last().x()) {
        out.push_back(src.last());
    }
    return out;
}

}  // namespace ens::ui
