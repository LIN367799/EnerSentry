// src/ui/auth/SessionLockDialog.h —— 会话超时锁屏（FR-AUTH-05）。
// exec() 返回 Accepted = 解锁成功；Rejected = 保持锁定（关闭窗口重开）。
#pragma once

#include <QDialog>

namespace ens::business {
class AuthManager;
}  // namespace ens::business

namespace Ui {
class SessionLockDialog;
}

namespace ens::ui {

class SessionLockDialog : public QDialog {
    Q_OBJECT
public:
    explicit SessionLockDialog(ens::business::AuthManager* auth, QWidget* parent = nullptr);
    ~SessionLockDialog() override;

private slots:
    void onUnlockClicked();

private:
    Ui::SessionLockDialog* ui;
    ens::business::AuthManager* m_auth;
};

}  // namespace ens::ui
