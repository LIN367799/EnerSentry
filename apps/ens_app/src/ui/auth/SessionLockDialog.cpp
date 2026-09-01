// src/ui/auth/SessionLockDialog.cpp —— 锁屏实现。
#include "auth/SessionLockDialog.h"
#include "ui_SessionLockDialog.h"

#include "AuthManager.h"

#include <QIcon>

namespace ens::ui {

SessionLockDialog::SessionLockDialog(ens::business::AuthManager* auth, QWidget* parent)
    : QDialog(parent), ui(new Ui::SessionLockDialog), m_auth(auth) {
    ui->setupUi(this);
    setWindowIcon(QIcon(QStringLiteral(":/icons/lock.svg")));
    ui->lblIcon->setPixmap(QIcon(QStringLiteral(":/icons/lock.svg")).pixmap(36, 36));
    connect(ui->btnUnlock, &QPushButton::clicked, this, &SessionLockDialog::onUnlockClicked);
    ui->editPass->setFocus();
}

SessionLockDialog::~SessionLockDialog() {
    delete ui;
}

void SessionLockDialog::onUnlockClicked() {
    if (!m_auth) { accept(); return; }
    if (m_auth->unlock(ui->editPass->text())) {
        accept();
    } else {
        ui->lblError->setProperty("role", "error");
        ui->lblError->setText(QStringLiteral("密码错误"));
        ui->editPass->clear();
        ui->editPass->setFocus();
    }
}

}  // namespace ens::ui
