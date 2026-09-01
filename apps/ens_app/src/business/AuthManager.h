// src/business/AuthManager.h —— L4 身份认证与会话（ENS-LLD-400 §4 最小版，Phase 4 切片 19）。
//
// 职责（切片 19 最小可用，对齐 FR-AUTH-01/02/05 + NFR-SEC-06 的 UI 前置部分）：
//   * 用户表加载（users.json：{users:[{username,password,role}]}，role ∈ Operator/Engineer/Admin）
//   * 登录/登出 + 当前用户/角色查询
//   * 会话锁定（FR-AUTH-05：15 分钟无操作自动锁定，由 GUI 侧 QTimer 轮询 idleSeconds 触发）
//   * 解锁（重新校验当前用户密码）
//
// 范围裁剪（V1.6+ 补齐，UI 已留 PermissionFilter 抽象）：
//   * 明文密码比对（V1.6 改密码哈希，NFR-SEC-06）
//   * 登录失败 5 次锁 15 分钟（NFR-SEC-06）
//   * 首登强改密（FR-AUTH-01）
//   * checkPermission 权限点校验 / 审计拦截 / ScopedAuthGuard
//
// 线程：仅主线程（UI 线程）使用，无内部锁。
#pragma once

#include <ens/export.hpp>   // ENS_BUSINESS_API（SHARED 导出宏）

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVector>

namespace ens::business {

// 三级角色（LLD-400 §4.1：Operator / Engineer / Admin，累积权限）
enum class UserRole : uint8_t {
    Operator = 0,
    Engineer = 1,
    Admin    = 2,
};

class ENS_BUSINESS_API AuthManager : public QObject {
    Q_OBJECT
public:
    explicit AuthManager(QObject* parent = nullptr);

    /// 从 users.json 加载用户表。失败/空表时回退内置默认用户（admin/operator），
    /// 返回 false 并 qWarning 提示（开发期可用；生产由 V1.6 用户管理保证文件存在）。
    bool loadUsersFromJson(const QString& path);

    /// 登录：用户名+密码校验通过 → 建立会话（角色、空闲计时重置）。
    /// @return true 成功；false 用户不存在/密码错误/已锁定
    bool login(const QString& username, const QString& password);
    void logout();

    bool isLoggedIn() const { return m_loggedIn; }
    QString currentUser() const { return m_currentUser; }
    UserRole currentRole() const { return m_currentRole; }
    int  userCount() const { return static_cast<int>(m_users.size()); }

    /// ── 会话锁定（FR-AUTH-05）──
    bool isLocked() const { return m_locked; }
    void lock();                        // 手动锁屏
    bool unlock(const QString& password);  // 解锁：重新校验当前用户密码
    void touchActivity();               // 任意操作重置空闲计时
    int  idleSeconds() const;           // 距上次活动秒数（GUI 1s 轮询，>=900 触发 lock）

signals:
    void loginChanged(const QString& user, int role);   // 登录成功（含解锁后恢复）
    void loggedOut();
    void lockedChanged(bool locked);

private:
    struct UserRec {
        QString  username;
        QString  password;   // 明文（V1.6 改哈希）
        UserRole role = UserRole::Operator;
    };
    const UserRec* findUser(const QString& username) const;

    QVector<UserRec> m_users;
    QString  m_currentUser;
    UserRole m_currentRole = UserRole::Operator;
    bool     m_loggedIn = false;
    bool     m_locked   = false;
    QElapsedTimer m_idle;   // 登录/解锁/touchActivity 时 restart()
};

}  // namespace ens::business
