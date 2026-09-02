// src/ui/controls/UserManagerDialog.h —— 用户管理对话框（切片 31，FR-AUTH-01 完整版）。
// 列表 + 新增（随机 salt 哈希）+ 删除（禁删自己）+ 改密（QInputDialog）+ 保存到 users.json。
// Admin-only 入口（MainWindow 菜单裁剪 + AuthManager::checkPermission 兜底）。
#pragma once

#include <QDialog>
#include <QStandardItemModel>

namespace ens::business {
class AuthManager;
}  // namespace ens::business

namespace Ui {
class UserManagerDialog;
}

namespace ens::ui {

class UserManagerDialog : public QDialog {
    Q_OBJECT
public:
    UserManagerDialog(ens::business::AuthManager* auth, const QString& usersPath,
                      QWidget* parent = nullptr);
    ~UserManagerDialog() override;

private slots:
    void onAddClicked();
    void onRemoveClicked();
    void onChangePassword();
    void onSaveClicked();

private:
    void refreshUsers();
    QString selectedUsername() const;

    Ui::UserManagerDialog* ui;
    ens::business::AuthManager* m_auth;
    QString m_usersPath;
    QStandardItemModel* m_model;
};

}  // namespace ens::ui
