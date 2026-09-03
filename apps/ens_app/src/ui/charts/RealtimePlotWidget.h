// src/ui/charts/RealtimePlotWidget.h —— L5 实时曲线渲染控件（ENS-LLD-503 / ADR-22）。
//
// 刷新纪律（ADR-22 硬约束）：
//   * 严禁"数据到达即 replot"——onNewSample 仅写 ChannelBuffer.pending（QWriteLocker）；
//   * 30Hz QTimer → onBatchRepaint：每通道 pending 经 RenderDownsampler::minMaxBucketDownSample
//     压缩到 ≤ MAX_POINTS_PER_CHANNEL → setData → replot(QCustomPlot::rpQueuedReplot)。
// 同屏 ≤8 通道目标 CPU <15%（切片 20 验收）。
//
// 切片 39（P1/P2 增强）：
//   * FR-RT-08 分轴：通道可分配左/右 yAxis（右侧懒建），rescale 按轴分组独立缩放；
//   * FR-RT-06 悬停读值：鼠标移动 → 最近通道数据点 → tracer + 文本（时间/通道/值）；
//   * FR-RT-07 标尺：setRulerEnabled 开垂直标尺线，随鼠标拖动并读全部通道值。
#pragma once

#include "charts/ChannelBuffer.h"

#include <QColor>
#include <QHash>
#include <QSharedPointer>
#include <QTimer>
#include <QWidget>

#include <cstdint>

class QCPAxis;
class QCPGraph;
class QCPItemStraightLine;
class QCPItemText;
class QCPItemTracer;
class QCustomPlot;
class QMouseEvent;

namespace ens::ui {

class RealtimePlotWidget : public QWidget {
    Q_OBJECT
public:
    static constexpr int MAX_POINTS_PER_CHANNEL = 2000;

    /// 通道 y 轴分配（FR-RT-08）
    enum class AxisSide { Left, Right };

    explicit RealtimePlotWidget(QWidget* parent = nullptr);

    // ── 通道管理（RealtimeChartWidget 按 DataBus 流出现顺序建通道，上限 8）──
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

    // ── 切片 39：分轴 / 悬停 / 标尺 ──
    /// 通道绑定 y 轴（默认 Left；切到 Right 时右侧轴懒建）
    void setChannelAxis(uint32_t pointId, AxisSide side);
    /// 开启/关闭垂直标尺（FR-RT-07）；开启后鼠标移动驱动标尺线与多通道读值
    void setRulerEnabled(bool on);
    bool rulerEnabled() const noexcept { return m_rulerOn; }
    /// 是否有通道绑定右轴（FR-RT-08 观测/测试）
    bool hasRightAxis() const;
    /// 测试钩子：等价于鼠标移动到像素 (xPix, yPix)（驱动悬停/标尺读值，免平台鼠标）
    void notifyMouseMoved(int xPix);
    /// 最近一次悬停/标尺文本（测试观测）
    QString hoverText() const noexcept { return m_hoverText; }

    /// 悬停行文本格式化（纯函数，单测友好）
    static QString formatHoverLine(qint64 tsMs, const QString& name, double value);

    /// 立即刷一帧（生产由 30Hz timer 驱动；测试/外部显式触发用）
    void refreshNow() { onBatchRepaint(); }

    /// FR-EXP-02：曲线区截图存 PNG（内部 QCustomPlot::savePng，含暗色样式）
    /// @param path 输出 .png 路径；@return true 成功
    bool savePng(const QString& path) const;

signals:
    void channelAdded(uint32_t pointId, const QString& name);   // ChartWidget 同步列表

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private slots:
    void onBatchRepaint();
    void onMouseMove(QMouseEvent* e);

private:
    /// 像素 x → 数据 x（悬停/标尺读值）
    void updateReadout(double xKey);
    /// 通道索引 → graph（m_buf 插入序与 graph 序一致）
    QCPGraph* graphAt(uint32_t pointId) const;
    /// 懒建右轴（样式同左轴 + 独立 label）
    QCPAxis* rightAxis();
    /// 移除随 graph 失效的 item（悬停 tracer/label）
    void detachReadoutItems();

    QCustomPlot* m_plot;
    QHash<uint32_t, QSharedPointer<ChannelBuffer>> m_buf;   // pointId → 通道
    QTimer m_timer;                          // 33ms ≈ 30Hz（PreciseTimer）
    QVector<QColor> m_palette;
    int m_nextColor = 0;
    bool m_active = true;

    // ── 切片 39 交互状态 ──
    QCPAxis*   m_rightAxis   = nullptr;   // 懒建右轴
    bool       m_rulerOn     = false;
    QString    m_hoverText;
    QCPItemTracer* m_tracer      = nullptr;   // 悬停十字（最近通道）
    QCPItemText*   m_hoverLabel  = nullptr;
    QCPItemStraightLine* m_rulerLine  = nullptr;   // 标尺垂线
    QCPItemText*   m_rulerLabel  = nullptr;
};

}  // namespace ens::ui
