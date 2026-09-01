// src/ui/charts/ChannelBuffer.h —— L5 每通道双缓冲（ENS-LLD-500 §3.1）。
// pending：采集侧 onNewSample 追加（QWriteLocker）；ready：渲染侧 30Hz 批处理降采样结果。
// 采集与渲染同主线程（DataBus broadcast 在主线程），锁为将来采集线程化兜底。
#pragma once

#include <QColor>
#include <QPointF>
#include <QReadWriteLock>
#include <QString>
#include <QVector>

namespace ens::ui {

struct ChannelBuffer {
    static constexpr int kMaxPending = 5000;   // 缓冲上限（超限丢最旧，ADR-22 防爆内存）

    QReadWriteLock rw;                 // 保护 pending（ready 仅渲染线程独占）
    QVector<QPointF> pending;          // 新样本队列（x=秒,y=工程值）
    QVector<QPointF> ready;            // 上一拍降采样输出（setData 源）
    QString name;
    QColor  color{0x4f, 0xc3, 0xf7};
    bool    visible = true;

    /// 追加样本（采集线程）；超过 kMaxPending 丢最旧
    void append(double tsSec, double value) {
        QWriteLocker lock(&rw);
        pending.push_back(QPointF(tsSec, value));
        if (pending.size() > kMaxPending) {
            pending.remove(0, pending.size() - kMaxPending);
        }
    }
};

}  // namespace ens::ui
