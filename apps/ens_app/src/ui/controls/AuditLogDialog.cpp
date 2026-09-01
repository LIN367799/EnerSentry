// src/ui/controls/AuditLogDialog.cpp —— 审计日志对话框实现（切片 30）。
#include "controls/AuditLogDialog.h"
#include "ui_AuditLogDialog.h"

#include "AuthManager.h"

#include <QDateTime>
#include <QHeaderView>

namespace ens::ui {

AuditLogDialog::AuditLogDialog(ens::business::AuthManager* auth, QWidget* parent)
    : QDialog(parent), ui(new Ui::AuditLogDialog), m_auth(auth) {
    ui->setupUi(this);

    m_model = new QStandardItemModel(this);
    m_model->setHorizontalHeaderLabels(
        {QStringLiteral("时间"), QStringLiteral("用户"), QStringLiteral("动作"),
         QStringLiteral("详情"), QStringLiteral("结果")});
    ui->tableAudit->setModel(m_model);
    ui->tableAudit->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->tableAudit->verticalHeader()->setVisible(false);

    connect(ui->btnRefresh, &QPushButton::clicked, this, &AuditLogDialog::onRefresh);
    connect(ui->btnClear, &QPushButton::clicked, this, &AuditLogDialog::onClear);
    connect(ui->btnClose, &QPushButton::clicked, this, &QDialog::accept);

    // Admin-only 清空（FR-AUTH-04 审计不可被普通角色篡改）
    const bool canManage = m_auth && m_auth->checkPermission(ens::business::perms::kAuthManage);
    ui->btnClear->setEnabled(canManage);

    fillModel();
}

AuditLogDialog::~AuditLogDialog() {
    delete ui;
}

void AuditLogDialog::onRefresh() {
    fillModel();
}

void AuditLogDialog::onClear() {
    if (m_auth) {
        m_auth->auditRecord(QStringLiteral("audit.clear"),
                            m_auth->currentUser(), true);   // 清空动作自身留痕
        m_auth->clearAudit();
    }
    fillModel();
}

void AuditLogDialog::fillModel() {
    m_model->removeRows(0, m_model->rowCount());
    if (!m_auth) return;
    const auto log = m_auth->auditLog();
    m_model->setRowCount(static_cast<int>(log.size()));
    for (int i = 0; i < static_cast<int>(log.size()); ++i) {
        const auto& e = log[static_cast<size_t>(i)];
        const QString ts = QDateTime::fromMSecsSinceEpoch(e.tsMs)
                               .toString(QStringLiteral("MM-dd HH:mm:ss"));
        m_model->setItem(i, 0, new QStandardItem(ts));
        m_model->setItem(i, 1, new QStandardItem(e.user));
        m_model->setItem(i, 2, new QStandardItem(e.action));
        m_model->setItem(i, 3, new QStandardItem(e.detail));
        auto* res = new QStandardItem(e.granted ? QStringLiteral("允许") : QStringLiteral("拒绝"));
        res->setForeground(e.granted ? QColor(0x66, 0xbb, 0x6a) : QColor(0xe9, 0x45, 0x60));
        m_model->setItem(i, 4, res);
    }
    ui->lblCount->setText(QStringLiteral("共 %1 条").arg(log.size()));
}

}  // namespace ens::ui
