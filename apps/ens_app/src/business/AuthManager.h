// src/business/AuthManager.h —— L4 身份认证与会话 + RBAC（ENS-LLD-400 §4，切片 19 最小版 + 切片 26 V1.6）。
//
// 职责：
//   * 用户表加载（users.json；缺失回退内置 admin/operator）
//   * 登录/登出 + 当前用户/角色查询
//   * 会话锁定（FR-AUTH-05：15 分钟无操作自动锁定，GUI 侧 QTimer 轮询 idleSeconds）
//   * 解锁（重新校验当前用户密码）
//   * RBAC（切片 26）：权限点 checkPermission（三级累积角色矩阵）+ 操作审计（环形缓冲）+ ScopedAuthGuard
//
// 权限点（字符串常量，业务侧按需扩展）：
//   kPermSboSelect / kPermSboOperate / kPermSboCancel —— SBO 控制（Engineer+）
//   kPermConfigView —— 参数配置页（Engineer+；Operator 只读不得改配置）
//   kPermAuthManage —— 用户管理（Admin，V1.6+ 预留）
//   kPermSystemExit —— 退出程序（全部角色允许）
//   kPermHistoryView / kPermDiagView —— 只读页（全部角色允许）
//
// 审计：内存环形缓冲（kAuditCap=1000），记录 login/logout/lock/unlock/权限拒绝/显式 auditRecord；
//   auditLog() 返回拷贝供管理视图/测试。
//
// 范围裁剪（V2.0+）：密码哈希（NFR-SEC-06）/ 登录失败 5 次锁定 / 首登强改密 / 用户管理 UI。
//
// 线程：仅主线程（UI 线程）使用，无内部锁。
#pragma once

#include <ens/export.hpp>   // ENS_BUSINESS_API（SHARED 导出宏）

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>
#include <vector>

namespace ens::business {

// 三级角色（LLD-400 §4.1：Operator / Engineer / Admin，累积权限）
enum class UserRole : uint8_t {
    Operator = 0,
    Engineer = 1,
    Admin    = 2,
};

/// 权限点常量（切片 26 RBAC）。用 const char* 避免 QString 静态初始化/MSVC inline 解析问题；
/// checkPermission 参数为 QString，const char* 隐式转换。
namespace perms {
inline const char* const kSboSelect  = "sbo.select";
inline const char* const kSboOperate = "sbo.operate";
inline const char* const kSboCancel  = "sbo.cancel";
inline const char* const kConfigView = "config.view";
inline const char* const kAuthManage = "auth.manage";
inline const char* const kSystemExit = "system.exit";
}  // namespace perms

/// 审计条目
struct AuditEntry {
    int64_t  tsMs    = 0;      // epoch ms
    QString  user;
    QString  action;           // login / logout / lock / unlock / 业务 action
    QString  detail;
    bool     granted = true;   // 审计拦截（checkPermission 拒绝）→ false
};

class AuthManager;   // ScopedAuthGuard 前向声明（RAII 守卫定义先于 AuthManager）

/// RAII 权限守卫（切片 26）：作用域内声明需要某权限
/// 构造时 checkPermission；析构时写审计（granted=结果）。
/// 用法：ScopedAuthGuard g(&auth, perms::kSboOperate, "sbo operate");
///       if (!g.granted()) return;
class ENS_BUSINESS_API ScopedAuthGuard {
public:
    ScopedAuthGuard(AuthManager* auth, const QString& perm, const QString& action);
    ~ScopedAuthGuard();
    bool granted() const { return m_granted; }
    ScopedAuthGuard(const ScopedAuthGuard&) = delete;
    ScopedAuthGuard& operator=(const ScopedAuthGuard&) = delete;
private:
    AuthManager* m_auth;
    QString m_perm;
    QString m_action;
    bool    m_granted = false;
};

class ENS_BUSINESS_API AuthManager : public QObject {
    Q_OBJECT
public:
    static constexpr int kAuditCap = 1000;
    // 登录失败锁定（FR-AUTH-06 / NFR-SEC-06，切片 28）
    static constexpr int    kMaxLoginFails = 5;          // 连续失败 N 次
    static constexpr qint64 kLockMs        = 15LL * 60 * 1000;   // 锁 15 分钟

    explicit AuthManager(QObject* parent = nullptr);

    /// 从 users.json 加载用户表。失败/空表时回退内置默认用户（admin/operator）。
    /// 密码字段支持两种格式：明文（旧文件兼容）或 sha256$<salt>$<hex>（NFR-SEC-06）。
    bool loadUsersFromJson(const QString& path);

    /// 登录：用户名+密码校验通过 → 建立会话（角色、空闲计时重置）。
    /// @return true 成功；false 用户不存在/密码错误/被失败锁定
    bool login(const QString& username, const QString& password);
    void logout();

    /// 用户当前失败锁定剩余秒数（>0 表示该用户名被锁定；UI 可提示）
    int lockRemainingSeconds(const QString& username) const;

    bool isLoggedIn() const { return m_loggedIn; }
    QString currentUser() const { return m_currentUser; }
    UserRole currentRole() const { return m_currentRole; }
    int  userCount() const { return static_cast<int>(m_users.size()); }

    // ── 用户管理（切片 31，FR-AUTH-01 完整版）──
    struct UserInfo {
        QString  username;
        UserRole role = UserRole::Operator;
        bool     mustChange = false;   ///< 切片 32：首登强改密标志
    };
    /// 用户列表（不含密码，供 UI 展示）
    QVector<UserInfo> listUsers() const;
    /// 新增用户：随机 salt + SHA-256 哈希存储
    bool addUser(const QString& username, const QString& password, UserRole role);
    /// 删除用户：禁删当前登录用户（防止自杀锁死会话）
    bool removeUser(const QString& username);
    /// 改密：随机 salt 重哈希（旧密码立即失效）；当前用户改密同时清除首登强改密标志
    bool changePassword(const QString& username, const QString& newPassword);
    /// 写回 users.json（哈希格式，UTF-8）；目录不存在自动创建
    bool saveUsersToJson(const QString& path);
    /// 设置/清除首登强改密标志（管理员分发初始密码后置位）
    bool setMustChange(const QString& username, bool must);
    /// 当前会话用户是否需改密（登录后由 UI 触发强制改密流程，FR-AUTH-01）
    bool requiresPasswordChange() const;

    /// ── 会话锁定（FR-AUTH-05）──
    bool isLocked() const { return m_locked; }
    void lock();                        // 手动锁屏
    bool unlock(const QString& password);  // 解锁：重新校验当前用户密码
    void touchActivity();               // 任意操作重置空闲计时
    int  idleSeconds() const;           // 距上次活动秒数

    // ── RBAC（切片 26，LLD-400 §4.2/§4.3）──
    /// 权限点校验：未登录/已锁定 → false；按三级累积角色矩阵判定。
    /// 拒绝时自动写审计（granted=false）。
    bool checkPermission(const QString& permPoint);

    /// 显式审计记录（业务动作，如 SBO 下发成功/失败）
    void auditRecord(const QString& action, const QString& detail = {},
                     bool granted = true);

    /// 审计日志拷贝（环形，时间升序）
    std::vector<AuditEntry> auditLog() const;
    /// 最近 N 条（时间降序）
    std::vector<AuditEntry> recentAudit(int n = 50) const;
    void clearAudit();

signals:
    void loginChanged(const QString& user, int role);   // 登录成功（含解锁后恢复）
    void loggedOut();
    void lockedChanged(bool locked);

private:
    struct UserRec {
        QString  username;
        QString  password;   // 明文（旧文件兼容）或 sha256$<salt>$<hex>（NFR-SEC-06）
        UserRole role = UserRole::Operator;
        bool     mustChange = false;   // 切片 32：首登强改密
    };
    struct FailRec {
        int    count        = 0;
        qint64 lockUntilMs  = 0;
    };
    const UserRec* findUser(const QString& username) const;
    /// 角色矩阵：该角色是否拥有权限点
    static bool roleHasPermission(UserRole role, const QString& permPoint);
    void appendAudit(const QString& user, const QString& action,
                     const QString& detail, bool granted);

    QVector<UserRec> m_users;
    QString  m_currentUser;
    UserRole m_currentRole = UserRole::Operator;
    bool     m_loggedIn = false;
    bool     m_locked   = false;
    QElapsedTimer m_idle;   // 登录/解锁/touchActivity 时 restart()

    QHash<QString, FailRec> m_fails;   // 登录失败计数/锁定（按用户名，FR-AUTH-06）

    std::vector<AuditEntry> m_audit;   // 环形（kAuditCap 截断）
    int m_auditHead = 0;               // 下一写入槽（环形）
};

}  // namespace ens::business
