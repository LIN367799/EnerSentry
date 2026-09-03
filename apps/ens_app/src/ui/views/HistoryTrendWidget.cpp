// src/ui/views/HistoryTrendWidget.cpp —— 历史趋势实现（切片 24 / 切片 34 导出）。
#include "views/HistoryTrendWidget.h"
#include "ui_HistoryTrendWidget.h"

#include "common/CsvWriter.h"
#include "IDataAccess.h"
#include "PointTable.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QVBoxLayout>

#include <filesystem>
#include <iomanip>
#include <sstream>

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
/// float → 精简十进制（%g 语义，避免尾零；工程值范围内无科学计数噪音）
std::string formatValue(float v) {
    std::ostringstream ss;
    ss << std::setprecision(6) << v;
    return ss.str();
}
/// Unix ms → 本地可读时间（导出列）
std::string formatLocalTime(uint64_t epochMs) {
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(epochMs));
    return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")).toStdString();
}
}  // namespace

HistoryTrendWidget::HistoryTrendWidget(ens::datahub::IDataAccess* dal,
                                       const std::shared_ptr<ens::protocol::PointTable>& pt,
                                       QWidget* parent)
    : QWidget(parent), ui(new Ui::HistoryTrendWidget), m_dal(dal), m_pt(pt) {
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
    connect(ui->btnExport, &QPushButton::clicked, this, &HistoryTrendWidget::onExportClicked);
    connect(ui->btnPng, &QPushButton::clicked, this, &HistoryTrendWidget::onPngClicked);
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

std::string HistoryTrendWidget::unitOf(uint32_t pointId) const {
    if (!m_pt) return {};
    const ens::protocol::PointRuntime* p = m_pt->pointIdOf(pointId);
    return p ? p->unit : std::string{};
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

    // 导出上下文（FR-EXP-01）：缓存最近一次成功查询；空结果禁用导出
    m_hasResult = !rows.empty();
    m_lastRows = rows;
    m_lastPointId = pid;
    m_lastPointName = ui->comboPoint->currentText().toStdString();
    m_lastGranText = ui->comboGran->currentText().toStdString();
    ui->btnExport->setEnabled(m_hasResult);
    ui->btnPng->setEnabled(m_hasResult);   // 切片 41：截图与 CSV 同启用条件

    ui->lblResult->setText(QStringLiteral("查询完成：%1 个聚合样本（点 %2，粒度 %3）%4")
                               .arg(rows.size())
                               .arg(pid)
                               .arg(ui->comboGran->currentText())
                               .arg(m_hasResult ? QStringLiteral("，可导出 CSV/PNG")
                                                : QStringLiteral("（无数据，不可导出）")));
}

void HistoryTrendWidget::onExportClicked() {
    if (!m_hasResult || m_lastRows.empty()) return;

    const QString defaultName =
        QStringLiteral("%1_%2.csv")
            .arg(QString::fromStdString(m_lastPointName).replace(QChar(' '), QStringLiteral("_")))
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出历史趋势 CSV"), defaultName, QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty()) return;   // 用户取消

    ens::ui::CsvWriter writer(std::filesystem::path(path.toStdWString()));
    if (!writer.open()) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("无法创建文件：\n%1").arg(path));
        return;
    }

    writer.writeRow({"时间戳", "测点ID", "测点名称", "数值", "单位"});
    const std::string unit = unitOf(m_lastPointId);
    for (const auto& r : m_lastRows) {
        writer.writeRow({formatLocalTime(r.timestamp),
                         std::to_string(r.pointId),
                         m_lastPointName,
                         formatValue(r.avgValue),
                         unit});
    }
    writer.close();

    ui->lblResult->setText(QStringLiteral("已导出 %1 行 → %2")
                               .arg(m_lastRows.size())
                               .arg(QDir::toNativeSeparators(path)));
}

void HistoryTrendWidget::onPngClicked() {
    // 切片 41：FR-EXP-02 曲线截图（QCustomPlot 离线渲染 PNG）
    if (!m_hasResult) return;
    QCustomPlot* plot = qobject_cast<QCustomPlot*>(ui->plotLayout->itemAt(0)->widget());
    if (!plot) return;
    const QString defaultName =
        QStringLiteral("%1_%2.png")
            .arg(QString::fromStdString(m_lastPointName).replace(QChar(' '), QStringLiteral("_")))
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出历史趋势 PNG"), defaultName, QStringLiteral("PNG 图片 (*.png)"));
    if (path.isEmpty()) return;
    if (plot->savePng(path)) {
        ui->lblResult->setText(QStringLiteral("已导出截图 → %1")
                                   .arg(QDir::toNativeSeparators(path)));
    } else {
        ui->lblResult->setText(QStringLiteral("截图导出失败：%1").arg(path));
    }
}

}  // namespace ens::ui
