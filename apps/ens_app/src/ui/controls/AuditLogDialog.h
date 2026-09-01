// src/ui/controls/AuditLogDialog.h —— 审计日志对话框（切片 30，V2 可视化）。
// 展示 AuthManager::auditLog（时间/用户/动作/详情/授权）；刷新 + 清空（Admin-only，
// 经 checkPermission(perms::kAuthManage)，拒绝时清空按钮禁用）。
#pragma once

#include <QDialog>
#include <QStandardItemModel>

namespace ens::business {
class AuthManager;
}  // namespace ens::business

namespace Ui {
class AuditLogDialog;
}

namespace ens::ui {

class AuditLogDialog : public QDialog {
    Q_OBJECT
public:
    explicit AuditLogDialog(ens::business::AuthManager* auth, QWidget* parent = nullptr);
    ~AuditLogDialog() override;

private slots:
    void onRefresh();
    void onClear();

private:
    void fillModel();

    Ui::AuditLogDialog* ui;
    ens::business::AuthManager* m_auth;
    QStandardItemModel* m_model;
};

}  // namespace ens::ui
