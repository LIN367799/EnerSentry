// src/ui/charts/RealtimePlotWidget.cpp —— 实时曲线渲染实现（ADR-22；切片 39 交互扩展）。
#include "charts/RealtimePlotWidget.h"

#include "charts/OpenGLDetector.h"
#include "charts/RenderDownsampler.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>

#include "qcustomplot.h"

namespace ens::ui {

namespace {

/// 通道 graph 数据容器中找 key 最近点（key 升序；dataCount==0 返 false）
/// @note QCustomPlot 2.1.1：graph->data() 返回 QSharedPointer 容器（findBegin 非 const）
bool nearestSample(QSharedPointer<QCPDataContainer<QCPGraphData>> data, double key,
                   double* outKey, double* outVal) {
    if (!data || data->size() == 0) return false;
    auto it = data->findBegin(key);                 // 首个 >= key
    if (it == data->end()) {
        --it;
    } else if (it != data->begin()) {
        auto prev = it;
        --prev;
        if (key - prev->key < it->key - key) it = prev;
    }
    *outKey = it->key;
    *outVal = it->value;
    return true;
}

QString fmtAxisMs(double keySeconds) {
    const qint64 ms = static_cast<qint64>(keySeconds * 1000.0);
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm:ss.zzz"));
}

QColor kItemTextColor = QColor(0xd8, 0xde, 0xe6);

}  // namespace

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

    // OpenGL 加速（无独显/远程会话自动回退软件渲染，HLD-UI §4.3）
    OpenGLDetector::applyTo(m_plot);

    // 调色板（暗色主题亮色系，8 通道上限）
    m_palette = {QColor(0x4f, 0xc3, 0xf7), QColor(0xef, 0x53, 0x50),
                 QColor(0x66, 0xbb, 0x6a), QColor(0xff, 0xa7, 0x26),
                 QColor(0xab, 0x47, 0xbc), QColor(0x26, 0xc6, 0xda),
                 QColor(0xee, 0xee, 0x58), QColor(0xec, 0x40, 0x7a)};

    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(33);   // ≈30Hz
    connect(&m_timer, &QTimer::timeout, this, &RealtimePlotWidget::onBatchRepaint);
    m_timer.start();

    // ── 切片 39：悬停读值（FR-RT-06）──
    m_plot->setMouseTracking(true);
    connect(m_plot, qOverload<QMouseEvent*>(&QCustomPlot::mouseMove),
            this, &RealtimePlotWidget::onMouseMove);
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

    emit channelAdded(pointId, name);
}

void RealtimePlotWidget::removeChannel(uint32_t pointId) {
    const int idx = m_buf.keys().indexOf(pointId);
    m_buf.remove(pointId);
    if (idx >= 0 && idx < m_plot->graphCount()) {
        m_plot->removeGraph(idx);
    }
    detachReadoutItems();   // 悬停 tracer 可能引用被删 graph
}

void RealtimePlotWidget::onNewSample(uint32_t pointId, double value, qint64 tsMs) {
    auto it = m_buf.find(pointId);
    if (it == m_buf.end()) return;
    (*it)->append(tsMs / 1000.0, value);   // x 轴用 epoch 秒（趋势时间轴）
}

void RealtimePlotWidget::setRefreshActive(bool active) {
    m_active = active;
    if (active) m_timer.start();
    else m_timer.stop();
}

void RealtimePlotWidget::clearAll() {
    m_buf.clear();
    m_plot->clearGraphs();
    detachReadoutItems();
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

    // 切片 39：rescale 按轴分组（左/右各自独立，FR-RT-08）
    bool hasLeft = false, hasRight = false;
    for (int gi = 0; gi < m_plot->graphCount(); ++gi) {
        QCPGraph* g = m_plot->graph(gi);
        if (!g || g->dataCount() == 0) continue;
        if (g->valueAxis() == m_plot->yAxis) hasLeft = true;
        else if (g->valueAxis() == m_rightAxis) hasRight = true;
    }
    if (hasLeft) m_plot->yAxis->rescale(true);
    if (hasRight && m_rightAxis) m_rightAxis->rescale(true);
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

// ─────────────────────────── 切片 39：分轴 / 悬停 / 标尺 ───────────────────────────

QCPGraph* RealtimePlotWidget::graphAt(uint32_t pointId) const {
    const int idx = m_buf.keys().indexOf(pointId);
    if (idx < 0 || idx >= m_plot->graphCount()) return nullptr;
    return m_plot->graph(idx);
}

QCPAxis* RealtimePlotWidget::rightAxis() {
    if (m_rightAxis) return m_rightAxis;
    m_rightAxis = m_plot->axisRect()->addAxis(QCPAxis::atRight);
    m_rightAxis->setBasePen(QPen(QColor(0x4a, 0x55, 0x60), 1));
    m_rightAxis->setTickPen(QPen(QColor(0x4a, 0x55, 0x60), 1));
    m_rightAxis->setSubTickPen(QPen(QColor(0x4a, 0x55, 0x60), 1));
    m_rightAxis->setLabelColor(QColor(0x9f, 0xb3, 0xc8));
    m_rightAxis->setTickLabelColor(QColor(0x9f, 0xb3, 0xc8));
    m_rightAxis->setLabel(QStringLiteral("工程值（右）"));
    return m_rightAxis;
}

void RealtimePlotWidget::setChannelAxis(uint32_t pointId, AxisSide side) {
    QCPGraph* g = graphAt(pointId);
    if (!g) return;
    if (side == AxisSide::Right && g->valueAxis() == m_plot->yAxis) {
        g->setValueAxis(rightAxis());
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    } else if (side == AxisSide::Left && g->valueAxis() == m_rightAxis) {
        g->setValueAxis(m_plot->yAxis);
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    }
}

bool RealtimePlotWidget::hasRightAxis() const {
    for (int gi = 0; gi < m_plot->graphCount(); ++gi) {
        QCPGraph* g = m_plot->graph(gi);
        if (g && g->valueAxis() == m_rightAxis) return true;
    }
    return false;
}

QString RealtimePlotWidget::formatHoverLine(qint64 tsMs, const QString& name, double value) {
    const QString ts = QDateTime::fromMSecsSinceEpoch(tsMs)
                           .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    return QStringLiteral("%1 | %2 = %3").arg(ts, name).arg(value, 0, 'f', 2);
}

void RealtimePlotWidget::notifyMouseMoved(int xPix) {
    if (m_buf.isEmpty()) return;
    const double xKey = m_plot->xAxis->pixelToCoord(static_cast<double>(xPix));
    updateReadout(xKey);
}

void RealtimePlotWidget::onMouseMove(QMouseEvent* e) {
    notifyMouseMoved(e->pos().x());
}

void RealtimePlotWidget::updateReadout(double xKey) {
    // 标尺模式：垂线 + 全部通道读值（FR-RT-07）
    if (m_rulerOn) {
        if (!m_rulerLine) {
            m_rulerLine = new QCPItemStraightLine(m_plot);
            m_rulerLine->point1->setTypeX(QCPItemPosition::ptAxisRectRatio);
            m_rulerLine->point2->setTypeX(QCPItemPosition::ptAxisRectRatio);
            m_rulerLine->point1->setTypeY(QCPItemPosition::ptAxisRectRatio);
            m_rulerLine->point2->setTypeY(QCPItemPosition::ptAxisRectRatio);
            m_rulerLine->setPen(QPen(QColor(0xe0, 0xc0, 0x60), 1, Qt::DashLine));
            m_rulerLabel = new QCPItemText(m_plot);
            m_rulerLabel->setColor(kItemTextColor);
            m_rulerLabel->setBrush(QBrush(QColor(0x1c, 0x21, 0x27)));
            m_rulerLabel->setPadding(QMargins(4, 2, 4, 2));
            m_rulerLabel->position->setType(QCPItemPosition::ptAxisRectRatio);
            m_rulerLabel->position->setCoords(0.985, 0.02);
            m_rulerLabel->setPositionAlignment(Qt::AlignRight | Qt::AlignTop);
        }
        // 垂线 ratio = 像素 x / 轴区宽（坐标已在数据坐标下，转 ratio 用 axisRect）
        const double xFrac = m_plot->xAxis->coordToPixel(xKey) /
                             static_cast<double>(m_plot->axisRect()->width());
        m_rulerLine->point1->setCoords(xFrac, 0);
        m_rulerLine->point2->setCoords(xFrac, 1);

        QString txt = QStringLiteral("t = %1").arg(fmtAxisMs(xKey));
        for (int gi = 0; gi < m_plot->graphCount(); ++gi) {
            QCPGraph* g = m_plot->graph(gi);
            if (!g || g->dataCount() == 0) continue;
            double k = 0, v = 0;
            if (nearestSample(g->data(), xKey, &k, &v)) {
                txt += QStringLiteral("\n%1 = %2").arg(g->name()).arg(v, 0, 'f', 2);
            }
        }
        m_rulerLabel->setText(txt);
        m_hoverText = txt;
        m_plot->replot(QCustomPlot::rpQueuedReplot);
        return;
    }

    // 悬停读值：最近通道最近点（FR-RT-06）
    QCPGraph* best = nullptr;
    double bestK = 0, bestV = 0, bestD = 1e18;
    for (int gi = 0; gi < m_plot->graphCount(); ++gi) {
        QCPGraph* g = m_plot->graph(gi);
        if (!g || g->dataCount() == 0) continue;
        double k = 0, v = 0;
        if (!nearestSample(g->data(), xKey, &k, &v)) continue;
        const double d = std::abs(k - xKey);
        if (d < bestD) {
            bestD = d; best = g; bestK = k; bestV = v;
        }
    }
    if (!best) return;

    if (!m_tracer) {
        m_tracer = new QCPItemTracer(m_plot);
        m_tracer->setSize(6);
        m_tracer->setPen(QPen(QColor(0xe0, 0xe0, 0xe0), 1));
        m_tracer->setBrush(QBrush(QColor(0xff, 0xff, 0xff)));
        m_tracer->setStyle(QCPItemTracer::tsCircle);   // QCustomPlot 2.1.1 tracer 专用枚举
        m_hoverLabel = new QCPItemText(m_plot);
        m_hoverLabel->setColor(kItemTextColor);
        m_hoverLabel->setBrush(QBrush(QColor(0x1c, 0x21, 0x27)));
        m_hoverLabel->setPadding(QMargins(4, 2, 4, 2));
        m_hoverLabel->position->setType(QCPItemPosition::ptAxisRectRatio);
        m_hoverLabel->position->setCoords(0.02, 0.02);
        m_hoverLabel->setPositionAlignment(Qt::AlignLeft | Qt::AlignTop);
    }
    m_tracer->setGraph(best);
    m_tracer->setGraphKey(bestK);
    m_tracer->setInterpolating(false);
    m_tracer->setVisible(true);
    m_hoverLabel->setText(formatHoverLine(static_cast<qint64>(bestK * 1000.0),
                                          best->name(), bestV));
    m_hoverLabel->setVisible(true);
    m_hoverText = m_hoverLabel->text();
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void RealtimePlotWidget::setRulerEnabled(bool on) {
    if (on == m_rulerOn) return;
    m_rulerOn = on;
    if (on) {
        m_plot->setMouseTracking(true);
        updateReadout(m_plot->xAxis->range().center());   // 默认置中
    } else {
        if (m_rulerLine) {
            m_plot->removeItem(m_rulerLine);
            m_rulerLine = nullptr;
        }
        if (m_rulerLabel) {
            m_plot->removeItem(m_rulerLabel);
            m_rulerLabel = nullptr;
        }
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    }
}

void RealtimePlotWidget::detachReadoutItems() {
    if (m_tracer) m_tracer->setGraph(nullptr);
    if (m_rulerLine) {
        m_plot->removeItem(m_rulerLine);
        m_rulerLine = nullptr;
    }
    if (m_rulerLabel) {
        m_plot->removeItem(m_rulerLabel);
        m_rulerLabel = nullptr;
    }
}

}  // namespace ens::ui
