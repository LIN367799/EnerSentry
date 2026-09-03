// src/ui/views/overview_widget.cpp —— 总览实现（切片 19/40）。
#include "views/overview_widget.h"
#include "ui_OverviewWidget.h"

#include "common/OverviewPoints.h"
#include "controls/SocGauge.h"
#include "controls/TempHeatBar.h"
#include "PointTable.h"

#include <QHBoxLayout>
#include <QLabel>

#include <algorithm>

namespace ens::ui {

OverviewWidget::OverviewWidget(ens::datahub::DataBus* bus,
                               const std::shared_ptr<ens::protocol::PointTable>& pt,
                               QWidget* parent)
    : QWidget(parent), ui(new Ui::OverviewWidget), m_bus(bus), m_pt(pt) {
    ui->setupUi(this);

    // ── 切片 40：SOC 仪表盘 + 簇最高温热力条 ──
    m_gauge = new SocGauge(this);
    ui->socLayout->addWidget(m_gauge);
    m_heat = new TempHeatBar(this);
    ui->heatLayout->addWidget(m_heat);

    buildPointIndex();
    if (m_pt && m_socIds.isEmpty() && m_maxTempByRack.isEmpty()) {
        // 点表存在但无 Rack-* 约定点：保留禁用态（控件空）
    }

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

void OverviewWidget::buildPointIndex() {
    if (!m_pt) return;
    m_socIds.clear();
    m_maxTempByRack.clear();
    m_rackOrder.clear();
    for (const auto* p : m_pt->allPoints()) {
        const OvrPointInfo info =
            ovrClassifyName(QString::fromStdString(p->pointName));
        if (info.kind == OvrKind::Soc) {
            m_socIds.push_back(p->pointId);
        } else if (info.kind == OvrKind::ClusterMaxTemp && info.rackNo > 0) {
            if (!m_maxTempByRack.contains(info.rackNo)) {
                m_maxTempByRack.insert(info.rackNo, p->pointId);
            }
        }
    }
    m_rackOrder = m_maxTempByRack.keys().toVector();   // QList<int> → QVector<int>
    std::sort(m_rackOrder.begin(), m_rackOrder.end());
}

void OverviewWidget::onRefreshUi() {
    const uint64_t count = m_sub.count();
    ui->lblCount->setText(QString::number(count));
    if (count > 0) {
        ui->lblLastPid->setText(QString::number(m_sub.lastPid()));
        ui->lblLastVal->setText(QString::number(static_cast<double>(m_sub.lastVal()), 'f', 2));
        static uint64_t s_prev = 0;
        const uint64_t delta = count - s_prev;
        s_prev = count;
        ui->lblRate->setText(QString::number(delta * 2));  // 500ms 采样 × 2 = 点/秒
    }

    // ── FR-OV-06：整站 SOC = 活跃 Rack SOC 算术平均 ──
    if (!m_socIds.isEmpty()) {
        int n = 0;
        double sum = 0.0;
        for (uint32_t pid : m_socIds) {
            float v = 0.0f;
            if (m_sub.cached(pid, &v)) {
                sum += qBound(0.0, static_cast<double>(v), 100.0);
                ++n;
            }
        }
        if (n > 0) {
            m_lastSoc = sum / n;
            m_gauge->setValue(m_lastSoc);
        }
    }

    // ── FR-OV-07：各簇最高温热力条 ──
    QVector<TempHeatBar::Cell> cells;
    cells.reserve(m_rackOrder.size());
    for (int rackNo : m_rackOrder) {
        TempHeatBar::Cell c;
        c.label = ovrRackLabel(rackNo);
        const auto it = m_maxTempByRack.constFind(rackNo);
        float v = 0.0f;
        if (it != m_maxTempByRack.constEnd() && m_sub.cached(it.value(), &v)) {
            c.temp = v;
            c.valid = true;
        }
        cells.push_back(c);
    }
    m_heatCount = cells.size();
    m_heat->setCells(cells);
}

}  // namespace ens::ui
