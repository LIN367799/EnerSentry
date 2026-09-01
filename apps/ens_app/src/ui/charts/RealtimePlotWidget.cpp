// src/ui/charts/RealtimePlotWidget.cpp —— 实时曲线渲染实现（ADR-22）。
#include "charts/RealtimePlotWidget.h"

#include "charts/RenderDownsampler.h"

#include <QHBoxLayout>
#include <QShowEvent>
#include <QVBoxLayout>

#include "qcustomplot.h"

namespace ens::ui {

RealtimePlotWidget::RealtimePlotWidget(QWidget* parent) : QWidget(parent) {
    m_plot = new QCustomPlot(this);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_plot);

    // 暗色图表底（与 theme.qss 协调；轴/网格色手调，不依赖 QSS）
    m_plot->setBackground(QBrush(QColor(0x14, 0x18, 0x1d)));
    m_plot->xAxis->setBasePen(QPen(QColor(0x4a, 0x55, 0x60), 1));
    m_plot->xAxis->setTickPen(QPen(QColor(0x4a, 0x55, 0x60), 1));
    m_plot->xAxis->setSubTickPen(QPen(QColor(0x4a, 0x55, 0x60), 1));
    m_plot->xAxis->setLabelColor(QColor(0x9f, 0xb3, 0xc8));
    m_plot->xAxis->setTickLabelColor(QColor(0x9f, 0xb3, 0xc8));
    m_plot->yAxis->setBasePen(QPen(QColor(0x4a, 0x55, 0x60), 1));
    m_plot->yAxis->setTickPen(QPen(QColor(0x4a, 0x55, 0x60), 1));
    m_plot->yAxis->setSubTickPen(QPen(QColor(0x4a, 0x55, 0x60), 1));
    m_plot->yAxis->setLabelColor(QColor(0x9f, 0xb3, 0xc8));
    m_plot->yAxis->setTickLabelColor(QColor(0x9f, 0xb3, 0xc8));
    m_plot->xAxis->setLabel(QStringLiteral("时间 (s)"));
    m_plot->yAxis->setLabel(QStringLiteral("工程值"));
    m_plot->axisRect()->setBackground(QBrush(QColor(0x1c, 0x21, 0x27)));

    // 调色板（暗色主题亮色系，8 通道上限）
    m_palette = {QColor(0x4f, 0xc3, 0xf7), QColor(0xef, 0x53, 0x50),
                 QColor(0x66, 0xbb, 0x6a), QColor(0xff, 0xa7, 0x26),
                 QColor(0xab, 0x47, 0xbc), QColor(0x26, 0xc6, 0xda),
                 QColor(0xee, 0xee, 0x58), QColor(0xec, 0x40, 0x7a)};

    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(33);   // ≈30Hz
    connect(&m_timer, &QTimer::timeout, this, &RealtimePlotWidget::onBatchRepaint);
    m_timer.start();
}

void RealtimePlotWidget::addChannel(uint32_t pointId, const QString& name, const QColor& color) {
    if (m_buf.contains(pointId)) return;
    auto buf = QSharedPointer<ChannelBuffer>::create();
    buf->name = name;
    buf->color = color.isValid() ? color : m_palette[m_nextColor % m_palette.size()];
    ++m_nextColor;
    m_buf.insert(pointId, buf);

    m_plot->addGraph();
    QCPGraph* g = m_plot->graph(m_plot->graphCount() - 1);
    g->setName(name);
    g->setPen(QPen(buf->color, 1.5));
    g->setLineStyle(QCPGraph::lsLine);
    g->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssNone));
    m_plot->legend->setVisible(true);
    m_plot->legend->setBrush(QBrush(QColor(0x1c, 0x21, 0x27)));
    m_plot->legend->setTextColor(QColor(0xd8, 0xde, 0xe6));
    m_plot->legend->setBorderPen(QPen(QColor(0x34, 0x3b, 0x47)));
}

void RealtimePlotWidget::removeChannel(uint32_t pointId) {
    const int idx = m_buf.keys().indexOf(pointId);
    m_buf.remove(pointId);
    if (idx >= 0 && idx < m_plot->graphCount()) {
        m_plot->removeGraph(idx);
    }
}

void RealtimePlotWidget::onNewSample(uint32_t pointId, double value, qint64 tsMs) {
    auto it = m_buf.find(pointId);
    if (it == m_buf.end()) return;
    (*it)->append(tsMs / 1000.0, value);   // x 轴用相对秒（时间轴连续，起始处有跳变不影响趋势）
}

void RealtimePlotWidget::setRefreshActive(bool active) {
    m_active = active;
    if (active) m_timer.start();
    else m_timer.stop();
}

void RealtimePlotWidget::clearAll() {
    m_buf.clear();
    m_plot->clearGraphs();
    m_plot->replot();
}

void RealtimePlotWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (m_active) m_timer.start();   // 恢复可见 → 重启刷新（显隐节流）
}

void RealtimePlotWidget::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    m_timer.stop();                  // 隐藏挂起：不空转渲染
}

void RealtimePlotWidget::onBatchRepaint() {
    if (m_buf.isEmpty()) return;
    const int pixelBudget = std::max(64, width() / std::max(1, m_buf.size()));
    const int target = std::min({MAX_POINTS_PER_CHANNEL, pixelBudget});

    for (auto it = m_buf.begin(); it != m_buf.end(); ++it) {
        ChannelBuffer& buf = *it.value();
        const int idx = m_buf.keys().indexOf(it.key());
        if (idx < 0 || idx >= m_plot->graphCount()) continue;
        QCPGraph* g = m_plot->graph(idx);
        if (!g) continue;

        {
            QWriteLocker lock(&buf.rw);
            buf.ready = (buf.pending.size() > target)
                ? RenderDownsampler::minMaxBucketDownSample(buf.pending, target)
                : buf.pending;
            buf.pending.clear();
        }
        if (buf.ready.isEmpty()) continue;

        // 提取 keys/values 一次拷贝 setData（QCustomPlot 要求已排序 keys）
        QVector<double> keys, vals;
        keys.reserve(buf.ready.size());
        vals.reserve(buf.ready.size());
        for (const QPointF& p : buf.ready) {
            keys.push_back(p.x());
            vals.push_back(p.y());
        }
        g->setData(keys, vals, /*alreadySorted=*/true);
    }
    m_plot->rescaleAxes(true);
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

}  // namespace ens::ui
