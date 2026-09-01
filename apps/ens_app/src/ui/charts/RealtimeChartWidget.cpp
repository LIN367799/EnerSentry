// src/ui/charts/RealtimeChartWidget.cpp —— 实时曲线容器实现。
#include "charts/RealtimeChartWidget.h"

#include "charts/RealtimePlotWidget.h"

#include <QLabel>
#include <QMetaObject>
#include <QVBoxLayout>

namespace ens::ui {

void RealtimeChartSubscriber::onSample(const ens::datahub::Sample& s) noexcept {
    // 任意线程回调：只转发（线程安全经 QueuedConnection）
    m_w->onSampleBridge(s);
}

RealtimeChartWidget::RealtimeChartWidget(ens::datahub::DataBus* bus, QWidget* parent)
    : QWidget(parent), m_bus(bus), m_plot(new RealtimePlotWidget(this)),
      m_sub(this) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_plot);

    auto* hint = new QLabel(QStringLiteral(
        "实时曲线（ADR-22：30Hz 批处理 + min/max 降采样保尖峰；自动跟踪前 %1 个活跃测点）")
                                .arg(kMaxChannels), this);
    hint->setStyleSheet(QStringLiteral("color: #8b949e; padding: 2px 6px;"));
    lay->addWidget(hint);

    if (m_bus) {
        m_handle = m_bus->subscribeWildcard(&m_sub);
    }
}

RealtimeChartWidget::~RealtimeChartWidget() {
    if (m_bus && m_handle != 0) {
        m_bus->unsubscribe(m_handle);
    }
}

void RealtimeChartWidget::onSampleBridge(const ens::datahub::Sample& s) {
    // 主线程投递（跨线程安全；样本高频时按 pointId 建通道一次后只转发）
    QMetaObject::invokeMethod(m_plot, [this, pid = s.pointId, v = double(s.value), ts = s.timestamp]() {
        if (!m_plot->hasChannel(pid) && m_plot->channelCount() < kMaxChannels) {
            m_plot->addChannel(pid, QStringLiteral("pt %1").arg(pid), QColor());
        }
        m_plot->onNewSample(pid, v, static_cast<qint64>(ts));
    }, Qt::QueuedConnection);
}

}  // namespace ens::ui
