// src/ui/views/DiagWidget.cpp —— 通信诊断实现（切片 23）。
#include "views/DiagWidget.h"
#include "ui_DiagWidget.h"

#include "charts/RealtimePlotWidget.h"

#include "IChannel.h"

#include <QVBoxLayout>

namespace ens::ui {

DiagWidget::DiagWidget(ens::channel::IChannel* ch, QWidget* parent)
    : QWidget(parent), ui(new Ui::DiagWidget), m_ch(ch) {
    ui->setupUi(this);

    // 吞吐曲线（复用 30Hz 批处理渲染控件）
    m_plot = new RealtimePlotWidget(this);
    ui->plotLayout->addWidget(m_plot);
    m_plot->addChannel(0xFFFF0001u, QStringLiteral("请求吞吐(次/s)"), QColor(0x4f, 0xc3, 0xf7));

    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &DiagWidget::onRefreshTick);
    m_timer.start();
}

DiagWidget::~DiagWidget() {
    m_timer.stop();
    delete ui;
}

void DiagWidget::onRefreshTick() {
    if (!m_ch) return;
    const auto& s = m_ch->getStats();
    const uint64_t total   = s.requestTotal.load(std::memory_order_relaxed);
    const uint64_t timeout = s.timeoutCount.load(std::memory_order_relaxed);
    const uint64_t crc     = s.crcErrorCount.load(std::memory_order_relaxed);
    const double   rate    = s.qualityPercent();

    ui->lblTotal->setText(QString::number(total));
    ui->lblTimeout->setText(QString::number(timeout));
    ui->lblCrc->setText(QString::number(crc));
    ui->lblRate->setText(QStringLiteral("%1%").arg(rate, 0, 'f', 1));

    // 吞吐 = 每秒请求增量（ts 用 tick 计数，避免真实时钟跳变）
    const uint64_t delta = total - m_lastTotal;
    m_lastTotal = total;
    m_plot->onNewSample(0xFFFF0001u, static_cast<double>(delta),
                        static_cast<qint64>(++m_tick) * 1000);
}

}  // namespace ens::ui
