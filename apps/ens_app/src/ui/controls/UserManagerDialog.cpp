// src/ui/controls/UserManagerDialog.cpp —— 用户管理对话框实现（切片 31）。
#include "controls/UserManagerDialog.h"
#include "ui_UserManagerDialog.h"

#include "AuthManager.h"

#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>

namespace ens::ui {

namespace {
QString roleText(ens::business::UserRole r) {
    switch (r) {
        case ens::business::UserRole::Admin:    return QStringLiteral("Admin");
        case ens::business::UserRole::Engineer: return QStringLiteral("Engineer");
        case ens::business::UserRole::Operator: return QStringLiteral("Operator");
    }
    return QStringLiteral("?");
}
}  // namespace

UserManagerDialog::UserManagerDialog(ens::business::AuthManager* auth,
                                     const QString& usersPath, QWidget* parent)
    : QDialog(parent), ui(new Ui::UserManagerDialog), m_auth(auth), m_usersPath(usersPath) {
    ui->setupUi(this);

    m_model = new QStandardItemModel(this);
    m_model->setHorizontalHeaderLabels(
        {QStringLiteral("用户名"), QStringLiteral("角色")});
    ui->tableUsers->setModel(m_model);
    ui->tableUsers->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->tableUsers->verticalHeader()->setVisible(false);

    connect(ui->btnAdd,     &QPushButton::clicked, this, &UserManagerDialog::onAddClicked);
    connect(ui->btnRemove,  &QPushButton::clicked, this, &UserManagerDialog::onRemoveClicked);
    connect(ui->btnChPwd,   &QPushButton::clicked, this, &UserManagerDialog::onChangePassword);
    connect(ui->btnSave,    &QPushButton::clicked, this, &UserManagerDialog::onSaveClicked);
    connect(ui->btnClose,   &QPushButton::clicked, this, &QDialog::accept);

    refreshUsers();
}

UserManagerDialog::~UserManagerDialog() {
    delete ui;
}

void UserManagerDialog::refreshUsers() {
    m_model->removeRows(0, m_model->rowCount());
    if (!m_auth) return;
    const auto users = m_auth->listUsers();
    m_model->setRowCount(users.size());
    for (int i = 0; i < users.size(); ++i) {
        m_model->setItem(i, 0, new QStandardItem(users[i].username));
        m_model->setItem(i, 1, new QStandardItem(roleText(users[i].role)));
    }
}

QString UserManagerDialog::selectedUsername() const {
    const QModelIndexList sel = ui->tableUsers->selectionModel()->selectedRows(0);
    return sel.isEmpty() ? QString() : sel.first().data().toString();
}

void UserManagerDialog::onAddClicked() {
    if (!m_auth) return;
    const QString name = ui->editUsername->text().trimmed();
    const QString pass = ui->editPassword->text();
    const auto role = static_cast<ens::business::UserRole>(ui->comboRole->currentIndex());
    if (!m_auth->addUser(name, pass, role)) {
        QMessageBox::warning(this, QStringLiteral("新增失败"),
                             QStringLiteral("用户名已存在或参数非法。"));
        return;
    }
    ui->editUsername->clear();
    ui->editPassword->clear();
    refreshUsers();
}

void UserManagerDialog::onRemoveClicked() {
    if (!m_auth) return;
    const QString name = selectedUsername();
    if (name.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先选中一个用户。"));
        return;
    }
    if (!m_auth->removeUser(name)) {
        QMessageBox::warning(this, QStringLiteral("删除失败"),
                             QStringLiteral("不能删除当前登录用户。"));
        return;
    }
    refreshUsers();
}

void UserManagerDialog::onChangePassword() {
    if (!m_auth) return;
    const QString name = selectedUsername();
    if (name.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先选中一个用户。"));
        return;
    }
    bool ok = false;
    const QString np = QInputDialog::getText(
        this, QStringLiteral("修改密码"),
        QStringLiteral("为 %1 设置新密码：").arg(name), QLineEdit::Password,
        QString(), &ok);
    if (ok && !np.isEmpty()) {
        m_auth->changePassword(name, np);
        QMessageBox::information(this, QStringLiteral("完成"),
                                 QStringLiteral("密码已修改（保存后生效）。"));
    }
}

void UserManagerDialog::onSaveClicked() {
    if (!m_auth) return;
    if (m_auth->saveUsersToJson(m_usersPath)) {
        QMessageBox::information(this, QStringLiteral("完成"),
                                 QStringLiteral("已保存到 %1").arg(m_usersPath));
    } else {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("无法写入 %1").arg(m_usersPath));
    }
}

}  // namespace ens::ui
