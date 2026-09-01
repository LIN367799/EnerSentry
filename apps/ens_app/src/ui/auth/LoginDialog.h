// src/ui/auth/LoginDialog.h —— L5 登录首屏（ENS-LLD-500 §2.1 LoginDialog / FR-AUTH-01）。
// 持 AuthManager*（依赖注入，不直接构造认证逻辑）；exec() 返回 Accepted 表示登录成功。
// 首登强改密（FR-AUTH-01）与失败 5 次锁定（NFR-SEC-06）属 V1.6（RBAC 引擎），本版仅校验。
#pragma once

#include <QDialog>

namespace ens::business {
class AuthManager;
}  // namespace ens::business

namespace Ui {
class LoginDialog;
}

namespace ens::ui {

class LoginDialog : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(ens::business::AuthManager* auth, QWidget* parent = nullptr);
    ~LoginDialog() override;

private slots:
    void onLoginClicked();
    void onCancelClicked();

private:
    void tryLogin();

    Ui::LoginDialog* ui;
    ens::business::AuthManager* m_auth;
};

}  // namespace ens::ui
