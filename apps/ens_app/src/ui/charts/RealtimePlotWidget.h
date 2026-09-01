// src/ui/charts/RealtimePlotWidget.h —— L5 实时曲线渲染控件（ENS-LLD-503 / ADR-22）。
//
// 刷新纪律（ADR-22 硬约束）：
//   * 严禁"数据到达即 replot"——onNewSample 仅写 ChannelBuffer.pending（QWriteLocker）；
//   * 30Hz QTimer → onBatchRepaint：每通道 pending 经 RenderDownsampler::minMaxBucketDownSample
//     压缩到 ≤ MAX_POINTS_PER_CHANNEL → setData → replot(QCustomPlot::rpQueuedReplot)。
// 同屏 ≤8 通道目标 CPU <15%（切片 20 验收）。
#pragma once

#include "charts/ChannelBuffer.h"

#include <QColor>
#include <QHash>
#include <QSharedPointer>
#include <QTimer>
#include <QWidget>

#include <cstdint>

class QCustomPlot;

namespace ens::ui {

class RealtimePlotWidget : public QWidget {
    Q_OBJECT
public:
    static constexpr int MAX_POINTS_PER_CHANNEL = 2000;

    explicit RealtimePlotWidget(QWidget* parent = nullptr);

    /// 通道管理（RealtimeChartWidget 按 DataBus 流出现顺序建通道，上限 8）
    void addChannel(uint32_t pointId, const QString& name, const QColor& color);
    void removeChannel(uint32_t pointId);
    bool hasChannel(uint32_t pointId) const { return m_buf.contains(pointId); }
    int  channelCount() const { return m_buf.size(); }

    /// 采集侧入口（主线程 onSample 桥）：仅缓冲，绝不 replot
    void onNewSample(uint32_t pointId, double value, qint64 tsMs);

    /// 窗口不可见时挂起刷新（LLD-500 §2.2 显隐节流）
    void setRefreshActive(bool active);

    /// 全通道清空（视图重建/测试用）
    void clearAll();

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private slots:
    void onBatchRepaint();

private:
    QCustomPlot* m_plot;
    // ChannelBuffer 含 QReadWriteLock（不可拷贝）→ QHash value 用 QSharedPointer
    QHash<uint32_t, QSharedPointer<ChannelBuffer>> m_buf;   // pointId → 通道
    QTimer m_timer;                          // 33ms ≈ 30Hz（PreciseTimer）
    QVector<QColor> m_palette;
    int m_nextColor = 0;
    bool m_active = true;
};

}  // namespace ens::ui
