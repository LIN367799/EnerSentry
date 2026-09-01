// src/ui/views/overview_widget.cpp —— 总览实现。
#include "views/overview_widget.h"
#include "ui_OverviewWidget.h"

#include <QLabel>

namespace ens::ui {

OverviewWidget::OverviewWidget(ens::datahub::DataBus* bus, QWidget* parent)
    : QWidget(parent), ui(new Ui::OverviewWidget), m_bus(bus) {
    ui->setupUi(this);
    if (m_bus) {
        m_handle = m_bus->subscribeWildcard(&m_sub);
    }
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &OverviewWidget::onRefreshUi);
    m_timer.start();
}

OverviewWidget::~OverviewWidget() {
    m_timer.stop();
    if (m_bus && m_handle != 0) {
        m_bus->unsubscribe(m_handle);
    }
    delete ui;
}

void OverviewWidget::onRefreshUi() {
    const uint64_t count = m_sub.count();
    ui->lblCount->setText(QString::number(count));
    if (count > 0) {
        ui->lblLastPid->setText(QString::number(m_sub.lastPid()));
        ui->lblLastVal->setText(QString::number(static_cast<double>(m_sub.lastVal()), 'f', 2));
        // 采样速率：最近 1s 增量（简单近似：每秒刷新差值）
        static uint64_t s_prev = 0;
        const uint64_t delta = count - s_prev;
        s_prev = count;
        ui->lblRate->setText(QString::number(delta * 2));  // 500ms 采样 × 2 = 点/秒
    }
}

}  // namespace ens::ui
