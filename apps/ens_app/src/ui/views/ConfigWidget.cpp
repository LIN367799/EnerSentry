// src/ui/views/ConfigWidget.cpp —— 参数配置实现（切片 23/41）。
#include "views/ConfigWidget.h"
#include "ui_ConfigWidget.h"

#include "common/ExportUtils.h"
#include "PointTable.h"

#include <QFileDialog>
#include <QHeaderView>
#include <QMessageBox>

namespace ens::ui {

namespace {
QString regTypeName(ens::protocol::RegisterType t) {
    switch (t) {
        case ens::protocol::RegisterType::HoldingRegister: return QStringLiteral("Holding");
        case ens::protocol::RegisterType::InputRegister:   return QStringLiteral("Input");
        case ens::protocol::RegisterType::Coil:            return QStringLiteral("Coil");
        case ens::protocol::RegisterType::DiscreteInput:   return QStringLiteral("Discrete");
    }
    return QStringLiteral("?");
}
}  // namespace

ConfigWidget::ConfigWidget(const std::shared_ptr<protocol::PointTable>& pt,
                           const QString& rulesPath, int ruleCount,
                           const QString& host, quint16 port, int pollMs,
                           const QString& ptPath, const QString& dataRoot, QWidget* parent)
    : QWidget(parent), ui(new Ui::ConfigWidget), m_rulesPath(rulesPath), m_ptPath(ptPath),
      m_dataRoot(dataRoot) {
    ui->setupUi(this);

    m_ptModel = new QStandardItemModel(this);
    m_ptModel->setHorizontalHeaderLabels(
        {QStringLiteral("pointId"), QStringLiteral("点名"), QStringLiteral("从站"),
         QStringLiteral("类型"), QStringLiteral("地址"), QStringLiteral("缩放"), QStringLiteral("单位")});
    ui->tablePoints->setModel(m_ptModel);
    ui->tablePoints->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->tablePoints->verticalHeader()->setVisible(false);

    fillPointTable(pt);
    ui->lblRules->setText(QStringLiteral("规则文件：%1\n已加载条数：%2")
                              .arg(rulesPath.isEmpty() ? QStringLiteral("（未启用）") : rulesPath)
                              .arg(ruleCount));
    ui->lblHost->setText(host);
    ui->lblPort->setText(QString::number(port));
    ui->lblPoll->setText(QStringLiteral("%1 ms").arg(pollMs));

    // ── 切片 41：导出/备份（FR-EXP-06/05）──
    connect(ui->btnExportCfg, &QPushButton::clicked, this, &ConfigWidget::onExportCfgClicked);
    connect(ui->btnBackupData, &QPushButton::clicked, this, &ConfigWidget::onBackupClicked);
    if (m_ptPath.isEmpty() && m_rulesPath.isEmpty()) {
        ui->btnExportCfg->setEnabled(false);
        ui->lblExportNote->setText(QStringLiteral("点表/规则源路径为空，配置导出不可用。"));
    }
    if (m_dataRoot.isEmpty()) {
        ui->btnBackupData->setEnabled(false);
        ui->lblExportNote->setText(QStringLiteral("未启用 --data-dir，历史数据备份不可用。"));
    }
}

ConfigWidget::~ConfigWidget() {
    delete ui;
}

void ConfigWidget::fillPointTable(const std::shared_ptr<protocol::PointTable>& pt) {
    if (!pt) {
        ui->lblPtSummary->setText(QStringLiteral("点表未加载"));
        return;
    }
    const auto points = pt->allPoints();
    m_ptModel->setRowCount(static_cast<int>(points.size()));
    for (int i = 0; i < static_cast<int>(points.size()); ++i) {
        const protocol::PointRuntime* p = points[static_cast<size_t>(i)];
        m_ptModel->setItem(i, 0, new QStandardItem(QString::number(p->pointId)));
        m_ptModel->setItem(i, 1, new QStandardItem(QString::fromStdString(p->pointName)));
        m_ptModel->setItem(i, 2, new QStandardItem(QString::number(p->slaveAddress)));
        m_ptModel->setItem(i, 3, new QStandardItem(regTypeName(p->regType)));
        m_ptModel->setItem(i, 4, new QStandardItem(QString::number(p->registerAddr)));
        m_ptModel->setItem(i, 5, new QStandardItem(QString::number(static_cast<double>(p->scaleFactor), 'g', 4)));
        m_ptModel->setItem(i, 6, new QStandardItem(QString::fromStdString(p->unit)));
    }
    ui->lblPtSummary->setText(QStringLiteral("共 %1 个测点").arg(points.size()));
}

void ConfigWidget::onExportCfgClicked() {
    // FR-EXP-06：点表 + 告警规则 JSON 导出（源文件拷贝到用户选目录）
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择配置导出目录"));
    if (dir.isEmpty()) return;
    QStringList errors;
    if (!m_ptPath.isEmpty()) {
        QString err;
        if (!copyFileToDir(m_ptPath, dir, &err)) errors << err;
    }
    if (!m_rulesPath.isEmpty()) {
        QString err;
        if (!copyFileToDir(m_rulesPath, dir, &err)) errors << err;
    }
    if (errors.isEmpty()) {
        ui->lblExportNote->setText(QStringLiteral("已导出配置到：%1").arg(dir));
    } else {
        QMessageBox::warning(this, QStringLiteral("导出配置"),
                             QStringLiteral("部分导出失败：\n%1").arg(errors.join(QLatin1Char('\n'))));
    }
}

void ConfigWidget::onBackupClicked() {
    // FR-EXP-05：历史/告警月库递归备份（<root>/history 与 <root>/alarm）
    if (m_dataRoot.isEmpty()) return;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择备份目标目录"));
    if (dir.isEmpty()) return;

    QStringList errors;
    QString err;
    const QString hist = m_dataRoot + QStringLiteral("/history");
    if (QFileInfo::exists(hist) && !copyDirRecursive(hist, dir + QStringLiteral("/history"), &err)) {
        errors << err;
    }
    const QString alarm = m_dataRoot + QStringLiteral("/alarm");
    if (QFileInfo::exists(alarm) && !copyDirRecursive(alarm, dir + QStringLiteral("/alarm"), &err)) {
        errors << err;
    }
    if (errors.isEmpty()) {
        ui->lblExportNote->setText(QStringLiteral("已备份历史数据到：%1").arg(dir));
    } else {
        QMessageBox::warning(this, QStringLiteral("备份历史数据"),
                             QStringLiteral("部分备份失败：\n%1").arg(errors.join(QLatin1Char('\n'))));
    }
}

}  // namespace ens::ui
