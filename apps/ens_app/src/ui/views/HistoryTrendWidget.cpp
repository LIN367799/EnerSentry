// src/ui/views/HistoryTrendWidget.cpp —— 历史趋势实现（切片 24）。
#include "views/HistoryTrendWidget.h"
#include "ui_HistoryTrendWidget.h"

#include "IDataAccess.h"
#include "PointTable.h"

#include <QDateTime>
#include <QVBoxLayout>

#include "qcustomplot.h"

namespace ens::ui {

namespace {
ens::datahub::HistoryGranularity granOf(int comboIndex) {
    return comboIndex == 1 ? ens::datahub::HistoryGranularity::Gran1m
                           : ens::datahub::HistoryGranularity::Gran1s;
}
qint64 rangeMsOf(int comboIndex) {
    switch (comboIndex) {
        case 1:  return 24LL * 3600 * 1000;
        case 2:  return 7LL * 24 * 3600 * 1000;
        default: return 3600LL * 1000;   // 1h
    }
}
}  // namespace

HistoryTrendWidget::HistoryTrendWidget(ens::datahub::IDataAccess* dal,
                                       const std::shared_ptr<ens::protocol::PointTable>& pt,
                                       QWidget* parent)
    : QWidget(parent), ui(new Ui::HistoryTrendWidget), m_dal(dal) {
    ui->setupUi(this);

    auto* plot = new QCustomPlot(this);
    ui->plotLayout->addWidget(plot);
    plot->setBackground(QBrush(QColor(0x14, 0x18, 0x1d)));
    plot->xAxis->setLabelColor(QColor(0x9f, 0xb3, 0xc8));
    plot->xAxis->setTickLabelColor(QColor(0x9f, 0xb3, 0xc8));
    plot->yAxis->setLabelColor(QColor(0x9f, 0xb3, 0xc8));
    plot->yAxis->setTickLabelColor(QColor(0x9f, 0xb3, 0xc8));
    plot->xAxis->setLabel(QStringLiteral("时间"));
    plot->yAxis->setLabel(QStringLiteral("工程值（avg）"));
    plot->axisRect()->setBackground(QBrush(QColor(0x1c, 0x21, 0x27)));
    plot->addGraph();
    plot->graph(0)->setPen(QPen(QColor(0x4f, 0xc3, 0xf7), 1.5));

    fillPoints(pt);
    connect(ui->btnQuery, &QPushButton::clicked, this, &HistoryTrendWidget::onQueryClicked);
}

HistoryTrendWidget::~HistoryTrendWidget() {
    delete ui;
}

void HistoryTrendWidget::fillPoints(const std::shared_ptr<ens::protocol::PointTable>& pt) {
    ui->comboPoint->clear();
    m_pointIds.clear();
    if (!pt) return;
    const auto points = pt->allPoints();
    for (const ens::protocol::PointRuntime* p : points) {
        ui->comboPoint->addItem(QStringLiteral("%1  %2").arg(p->pointId)
                                    .arg(QString::fromStdString(p->pointName)));
        m_pointIds.push_back(p->pointId);
    }
}

void HistoryTrendWidget::onQueryClicked() {
    if (!m_dal || m_pointIds.empty()) {
        ui->lblResult->setText(QStringLiteral("无可查询测点或历史库未启用（--data-dir）"));
        return;
    }
    const int idx = ui->comboPoint->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(m_pointIds.size())) return;
    const uint32_t pid = m_pointIds[static_cast<size_t>(idx)];
    const auto gran = granOf(ui->comboGran->currentIndex());

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 begin = now - rangeMsOf(ui->comboRange->currentIndex());
    const auto rows = m_dal->queryRange(pid, static_cast<uint64_t>(begin),
                                        static_cast<uint64_t>(now), gran);

    QCustomPlot* plot = qobject_cast<QCustomPlot*>(ui->plotLayout->itemAt(0)->widget());
    if (!plot) return;
    QVector<double> xs, ys;
    xs.reserve(static_cast<int>(rows.size()));
    ys.reserve(static_cast<int>(rows.size()));
    for (const auto& r : rows) {
        xs.push_back(static_cast<double>(r.timestamp) / 1000.0);   // 秒
        ys.push_back(static_cast<double>(r.avgValue));
    }
    plot->graph(0)->setData(xs, ys, /*alreadySorted=*/true);
    plot->rescaleAxes();
    plot->replot(QCustomPlot::rpQueuedReplot);

    ui->lblResult->setText(QStringLiteral("查询完成：%1 个聚合样本（点 %2，粒度 %3）")
                               .arg(rows.size())
                               .arg(pid)
                               .arg(ui->comboGran->currentText()));
}

}  // namespace ens::ui
