// src/ui/controls/AlarmReplayDialog.cpp —— 告警回放实现（切片 38）。
#include "controls/AlarmReplayDialog.h"
#include "ui_AlarmReplayDialog.h"

#include "Sample.h"

#include <QColor>
#include <QDateTime>

#include "qcustomplot.h"

namespace ens::ui {

namespace {
QString fmtTime(uint64_t epochMs) {
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(epochMs))
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}
}  // namespace

AlarmReplayDialog::AlarmReplayDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::AlarmReplayDialog) {
    ui->setupUi(this);

    auto* plot = new QCustomPlot(this);
    ui->plotLayout->addWidget(plot);
    plot->setBackground(QBrush(QColor(0x14, 0x18, 0x1d)));
    plot->xAxis->setLabelColor(QColor(0x9f, 0xb3, 0xc8));
    plot->xAxis->setTickLabelColor(QColor(0x9f, 0xb3, 0xc8));
    plot->yAxis->setLabelColor(QColor(0x9f, 0xb3, 0xc8));
    plot->yAxis->setTickLabelColor(QColor(0x9f, 0xb3, 0xc8));
    plot->xAxis->setLabel(QStringLiteral("相对时间（秒，0 = 告警时刻）"));
    plot->yAxis->setLabel(QStringLiteral("工程值"));
    plot->axisRect()->setBackground(QBrush(QColor(0x1c, 0x21, 0x27)));
    plot->addGraph();
    plot->graph(0)->setPen(QPen(QColor(0xef, 0x9f, 0x27), 1.5));   // 琥珀色（Critical 回放）
    plot->graph(0)->setLineStyle(QCPGraph::lsLine);

    connect(ui->btnClose, &QPushButton::clicked, this, &AlarmReplayDialog::close);
}

AlarmReplayDialog::~AlarmReplayDialog() {
    delete ui;
}

void AlarmReplayDialog::setData(uint32_t pointId, const QString& pointName,
                                uint64_t triggerTime,
                                const std::vector<ens::datahub::Sample>& samples) {
    const QString name = pointName.isEmpty()
        ? QString::number(pointId)
        : QStringLiteral("%1 (id=%2)").arg(pointName).arg(pointId);
    ui->lblTitle->setText(QStringLiteral("%1 · 告警时刻 %2 · ±30s 高频回放")
                              .arg(name, fmtTime(triggerTime)));

    auto* plot = qobject_cast<QCustomPlot*>(ui->plotLayout->itemAt(0)->widget());
    if (!plot) return;
    QVector<double> xs, ys;
    xs.reserve(static_cast<int>(samples.size()));
    ys.reserve(static_cast<int>(samples.size()));
    for (const auto& s : samples) {
        // x：相对告警时刻的秒（告警前为负、后为正）；y：工程值
        xs.push_back((static_cast<double>(s.timestamp) - static_cast<double>(triggerTime)) / 1000.0);
        ys.push_back(static_cast<double>(s.value));
    }
    plot->graph(0)->setData(xs, ys, /*alreadySorted=*/true);
    plot->rescaleAxes();
    if (xs.isEmpty()) {
        // 空区间防 rescale 警告：给个占位范围
        plot->xAxis->setRange(-30, 30);
        plot->yAxis->setRange(-1, 1);
    }
    plot->replot(QCustomPlot::rpQueuedReplot);

    if (samples.empty()) {
        ui->lblHint->setText(
            QStringLiteral("L1 内存无该区间数据（告警已超过滚动窗口约 1h，或测点未注册）。"
                           "黑匣子文件级回放未实现。"));
    } else {
        ui->lblHint->setText(QStringLiteral("共 %1 个样本（%2 ms 高频）。")
                                 .arg(samples.size())
                                 .arg(samples.size() > 1
                                          ? QString::number(samples[1].timestamp -
                                                            samples[0].timestamp)
                                          : QStringLiteral("-")));
    }
}

}  // namespace ens::ui
