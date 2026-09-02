// src/ui/auth/LoginDialog.cpp —— 登录首屏实现。
#include "auth/LoginDialog.h"
#include "ui_LoginDialog.h"

#include "AuthManager.h"

#include <QIcon>
#include <QInputDialog>
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
        // 切片 32：首登强改密（FR-AUTH-01）——标志用户必须改密后才可进入；取消/失败 → 登出拒绝
        if (m_auth->requiresPasswordChange() && !forcePasswordChange()) {
            m_auth->logout();
            ui->lblError->setProperty("role", "error");
            ui->lblError->setText(QStringLiteral("首次登录必须修改密码后才能进入系统"));
            ui->editPass->clear();
            ui->editPass->setFocus();
            return;
        }
        accept();
    } else {
        ui->lblError->setProperty("role", "error");
        ui->lblError->setText(QStringLiteral("用户名或密码错误"));
        ui->editPass->clear();
        ui->editPass->setFocus();
    }
}

bool LoginDialog::forcePasswordChange() {
    while (true) {
        bool ok = false;
        const QString np = QInputDialog::getText(
            this, QStringLiteral("首次登录须修改密码"),
            QStringLiteral("请输入新密码："), QLineEdit::Password, QString(), &ok);
        if (!ok || np.isEmpty()) return false;   // 取消/空 → 拒绝进入
        const QString np2 = QInputDialog::getText(
            this, QStringLiteral("确认新密码"),
            QStringLiteral("请再次输入新密码："), QLineEdit::Password, QString(), &ok);
        if (!ok) return false;
        if (np == np2) {
            return m_auth && m_auth->changePassword(m_auth->currentUser(), np);
        }
        QMessageBox::warning(this, QStringLiteral("密码不一致"),
                             QStringLiteral("两次输入的密码不一致，请重新设置。"));
    }
}

}  // namespace ens::ui
