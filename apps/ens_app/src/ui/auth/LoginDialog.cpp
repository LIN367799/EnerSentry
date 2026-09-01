// src/ui/auth/LoginDialog.cpp —— 登录首屏实现。
#include "auth/LoginDialog.h"
#include "ui_LoginDialog.h"

#include "AuthManager.h"

#include <QIcon>
#include <QMessageBox>

namespace ens::ui {

LoginDialog::LoginDialog(ens::business::AuthManager* auth, QWidget* parent)
    : QDialog(parent), ui(new Ui::LoginDialog), m_auth(auth) {
    ui->setupUi(this);
    setWindowIcon(QIcon(QStringLiteral(":/icons/app_logo.svg")));
    ui->lblLogo->setPixmap(
        QIcon(QStringLiteral(":/icons/app_logo.svg")).pixmap(48, 48));

    connect(ui->btnLogin, &QPushButton::clicked, this, &LoginDialog::onLoginClicked);
    connect(ui->btnCancel, &QPushButton::clicked, this, &LoginDialog::onCancelClicked);
    // 回车默认触发默认按钮（btnLogin，.ui 已设 default）
    ui->editUser->setFocus();
}

LoginDialog::~LoginDialog() {
    delete ui;
}

void LoginDialog::onLoginClicked() {
    tryLogin();
}

void LoginDialog::onCancelClicked() {
    reject();
}

void LoginDialog::tryLogin() {
    const QString user = ui->editUser->text().trimmed();
    const QString pass = ui->editPass->text();
    if (user.isEmpty() || pass.isEmpty()) {
        ui->lblError->setProperty("role", "error");
        ui->lblError->setText(QStringLiteral("请输入用户名和密码"));
        return;
    }
    if (!m_auth) {
        // 认证未初始化：开发期容错直接放行（正常部署不会走到）
        accept();
        return;
    }
    if (m_auth->login(user, pass)) {
        accept();
    } else {
        ui->lblError->setProperty("role", "error");
        ui->lblError->setText(QStringLiteral("用户名或密码错误"));
        ui->editPass->clear();
        ui->editPass->setFocus();
    }
}

}  // namespace ens::ui
