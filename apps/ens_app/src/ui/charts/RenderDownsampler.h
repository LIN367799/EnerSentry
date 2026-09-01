// src/ui/charts/RenderDownsampler.h —— L5 渲染降采样（LLD-500 §5.2 / ADR-22）。
// min/max 桶降采样：把任意规模的时间序列压缩到 ≤target 个点，同时**保留尖峰**
// （每桶输出 min-y 与 max-y 两个点；末端补原始最后点保时间轴连续）。
// 用途：RealtimePlotWidget 30Hz 批处理把 pending(≤5000) 压缩到 ≤MAX_POINTS_PER_CHANNEL。
// 纯算法无 Qt 依赖（QPointF 除外），可单测。
#pragma once

#include <QPointF>
#include <QVector>

namespace ens::ui {

class RenderDownsampler {
public:
    /// @param src 按 x 升序的时间序列
    /// @param target 目标点数上限（<2 或 src.size()<=target 时原样返回）
    /// @return 降采样结果（≤ target 点；含尖峰 + 末端点）
    static QVector<QPointF> minMaxBucketDownSample(const QVector<QPointF>& src, int target);
};

}  // namespace ens::ui
