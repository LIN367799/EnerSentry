// src/ui/auth/LoginDialog.h —— L5 登录首屏（ENS-LLD-500 §2.1 LoginDialog / FR-AUTH-01）。
// 持 AuthManager*（依赖注入，不直接构造认证逻辑）；exec() 返回 Accepted 表示登录成功。
// 首登强改密（FR-AUTH-01，切片 32）：requiresPasswordChange() 用户须改密后才可进入。
#pragma once

#include <QDialog>
#include <memory>

namespace ens::business {
class AuthManager;
}  // namespace ens::business

namespace ens::ui {
class WindowChrome;
}  // namespace ens::ui

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
    bool forcePasswordChange();   // 切片 32：新密码两次输入校验（取消/失败返 false）

    Ui::LoginDialog* ui;
    ens::business::AuthManager* m_auth;
    std::unique_ptr<WindowChrome> m_chrome;   // 切片 45：Frameless 接管
};

}  // namespace ens::ui
