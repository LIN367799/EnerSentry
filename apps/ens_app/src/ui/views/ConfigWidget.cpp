// src/ui/views/ConfigWidget.cpp —— 参数配置实现（切片 23 骨架）。
#include "views/ConfigWidget.h"
#include "ui_ConfigWidget.h"

#include "PointTable.h"

#include <QHeaderView>

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
                           const QString& host, quint16 port, int pollMs, QWidget* parent)
    : QWidget(parent), ui(new Ui::ConfigWidget) {
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

}  // namespace ens::ui
