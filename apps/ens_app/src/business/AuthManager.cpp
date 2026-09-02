// src/business/AuthManager.cpp —— 认证 + RBAC（切片 19 最小版 + 切片 26 V1.6：权限/审计/守卫）。
#include "AuthManager.h"

#include <QDateTime>
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QStringList>

#include <algorithm>
#include <initializer_list>
#include <tuple>

namespace ens::business {

namespace {

// 内置默认用户（users.json 缺失/空表时的开发期回退）。
// 密码为 sha256$<salt>$<hex> 哈希（NFR-SEC-06，切片 28；salt 固定仅演示用，
// 生产用户由用户管理 UI 用随机 salt 生成）。
const std::initializer_list<std::tuple<const char*, const char*, const char*>>
    kDefaultUsers = {
        {"admin",    "sha256$ens-salt-v1$21888813a45eed2911e66baa92ca2cc6269ce425b8d3ff2b303ce139a03283e1", "Admin"},
        {"operator", "sha256$ens-salt-v1$0acfaa9f0eac4258061f2521fabb91108bb97f07271d4acc2d256e16d4d03a3b", "Operator"},
};

UserRole roleFromString(const QString& s) {
    const QString low = s.trimmed().toLower();
    if (low == QStringLiteral("admin"))    return UserRole::Admin;
    if (low == QStringLiteral("engineer")) return UserRole::Engineer;
    return UserRole::Operator;
}

QString sha256Hex(const QByteArray& data) {
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

/// 密码校验：支持 sha256$salt$hash（NFR-SEC-06）与明文（旧 users.json 兼容）
bool verifyPassword(const QString& stored, const QString& password) {
    if (stored.startsWith(QStringLiteral("sha256$"))) {
        const QStringList parts = stored.split(QLatin1Char('$'));
        if (parts.size() != 3) return false;
        const QString expect = parts.at(2);
        const QString actual = sha256Hex(parts.at(1).toUtf8() + password.toUtf8());
        return actual == expect;
    }
    return stored == password;
}

}  // namespace

AuthManager::AuthManager(QObject* parent) : QObject(parent) {}

const AuthManager::UserRec* AuthManager::findUser(const QString& username) const {
    const auto it = std::find_if(m_users.cbegin(), m_users.cend(),
        [&](const UserRec& u) { return u.username == username; });
    return (it != m_users.cend()) ? &(*it) : nullptr;
}

bool AuthManager::loadUsersFromJson(const QString& path) {
    m_users.clear();
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonArray arr = doc.object().value(QStringLiteral("users")).toArray();
            for (const QJsonValue& v : arr) {
                const QJsonObject o = v.toObject();
                UserRec rec;
                rec.username  = o.value(QStringLiteral("username")).toString();
                rec.password  = o.value(QStringLiteral("password")).toString();
                rec.role      = roleFromString(o.value(QStringLiteral("role")).toString());
                rec.mustChange = o.value(QStringLiteral("mustChange")).toBool(false);   // 切片 32
                if (rec.username.isEmpty() || rec.password.isEmpty()) continue;
                m_users.push_back(rec);
            }
        }
    }

    if (m_users.isEmpty()) {
        for (const auto& [name, pass, role] : kDefaultUsers) {
            m_users.push_back(UserRec{QString::fromLatin1(name),
                                      QString::fromLatin1(pass),
                                      roleFromString(QString::fromLatin1(role))});
        }
        qWarning("AuthManager: users.json 缺失或为空，回退内置默认用户（%d 个）", m_users.size());
        return false;
    }
    return true;
}

bool AuthManager::login(const QString& username, const QString& password) {
    if (m_locked) return false;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    // FR-AUTH-06：失败锁定检查（该用户名锁定中 → 拒绝）
    const auto failIt = m_fails.constFind(username);
    if (failIt != m_fails.constEnd() && failIt->lockUntilMs > nowMs) {
        appendAudit(username, QStringLiteral("login"), QStringLiteral("locked"), false);
        return false;
    }

    const UserRec* u = findUser(username);
    if (!u || !verifyPassword(u->password, password)) {
        FailRec& f = m_fails[username];
        ++f.count;
        if (f.count >= kMaxLoginFails) {
            f.lockUntilMs = nowMs + kLockMs;
            f.count = 0;
            appendAudit(username, QStringLiteral("login"), QStringLiteral("locked-5x"), false);
        } else {
            appendAudit(username, QStringLiteral("login"), QStringLiteral("rejected"), false);
        }
        return false;
    }
    m_fails.remove(username);   // 成功登录清失败计数
    m_currentUser = username;
    m_currentRole = u->role;
    m_loggedIn = true;
    m_locked = false;
    m_idle.restart();
    appendAudit(m_currentUser, QStringLiteral("login"), QStringLiteral("ok"), true);
    emit loginChanged(m_currentUser, static_cast<int>(m_currentRole));
    return true;
}

int AuthManager::lockRemainingSeconds(const QString& username) const {
    const auto it = m_fails.constFind(username);
    if (it == m_fails.constEnd()) return 0;
    const qint64 remain = it->lockUntilMs - QDateTime::currentMSecsSinceEpoch();
    return remain > 0 ? static_cast<int>(remain / 1000) : 0;
}

void AuthManager::logout() {
    if (m_loggedIn) {
        appendAudit(m_currentUser, QStringLiteral("logout"), {}, true);
    }
    m_loggedIn = false;
    m_locked = false;
    m_currentUser.clear();
    m_currentRole = UserRole::Operator;
    emit loggedOut();
}

void AuthManager::lock() {
    if (!m_loggedIn) return;
    m_locked = true;
    appendAudit(m_currentUser, QStringLiteral("lock"), {}, true);
    emit lockedChanged(true);
}

bool AuthManager::unlock(const QString& password) {
    if (!m_loggedIn || !m_locked) return false;
    const UserRec* u = findUser(m_currentUser);
    if (!u || !verifyPassword(u->password, password)) {
        appendAudit(m_currentUser, QStringLiteral("unlock"), QStringLiteral("rejected"), false);
        return false;
    }
    m_locked = false;
    m_idle.restart();
    appendAudit(m_currentUser, QStringLiteral("unlock"), {}, true);
    emit lockedChanged(false);
    return true;
}

void AuthManager::touchActivity() {
    m_idle.restart();
}

int AuthManager::idleSeconds() const {
    // 未登录不计时；登录后 QElapsedTimer 自登录时刻起，touchActivity 重置
    if (!m_loggedIn) return 0;
    return static_cast<int>(m_idle.elapsed() / 1000);
}

// ── RBAC（切片 26）──

bool AuthManager::roleHasPermission(UserRole role, const QString& permPoint) {
    // 累积矩阵：Admin=全量；Engineer=控制+配置+只读+退出；Operator=只读+退出
    if (role == UserRole::Admin) return true;
    if (role == UserRole::Engineer) {
        return permPoint != perms::kAuthManage;
    }
    // Operator：只读页 + 退出
    return permPoint == perms::kSystemExit
        || permPoint == QStringLiteral("history.view")
        || permPoint == QStringLiteral("diag.view")
        || permPoint == QStringLiteral("trend.view")
        || permPoint == QStringLiteral("alarm.view")
        || permPoint == QStringLiteral("overview.view");
}

bool AuthManager::checkPermission(const QString& permPoint) {
    const bool ok = m_loggedIn && !m_locked
                    && roleHasPermission(m_currentRole, permPoint);
    if (!ok) {
        appendAudit(m_currentUser, QStringLiteral("perm:") + permPoint,
                    QStringLiteral("denied"), false);
    }
    return ok;
}

void AuthManager::auditRecord(const QString& action, const QString& detail, bool granted) {
    appendAudit(m_currentUser, action, detail, granted);
}

void AuthManager::appendAudit(const QString& user, const QString& action,
                              const QString& detail, bool granted) {
    AuditEntry e;
    e.tsMs    = QDateTime::currentMSecsSinceEpoch();
    e.user    = user;
    e.action  = action;
    e.detail  = detail;
    e.granted = granted;
    m_audit.push_back(e);
    if (static_cast<int>(m_audit.size()) > kAuditCap) {
        m_audit.erase(m_audit.begin());   // 环形截断（1000 上限，低频业务审计可接受 O(n)）
    }
}

std::vector<AuditEntry> AuthManager::auditLog() const {
    return m_audit;   // 时间升序
}

std::vector<AuditEntry> AuthManager::recentAudit(int n) const {
    std::vector<AuditEntry> out;
    const int take = std::min(n, static_cast<int>(m_audit.size()));
    for (int i = static_cast<int>(m_audit.size()) - take; i < static_cast<int>(m_audit.size()); ++i) {
        out.push_back(m_audit[static_cast<size_t>(i)]);
    }
    return out;   // 时间降序（最近在前）
}

void AuthManager::clearAudit() {
    m_audit.clear();
}

// ── ScopedAuthGuard ──

ScopedAuthGuard::ScopedAuthGuard(AuthManager* auth, const QString& perm, const QString& action)
    : m_auth(auth), m_perm(perm), m_action(action) {
    m_granted = m_auth ? m_auth->checkPermission(m_perm) : false;
}

ScopedAuthGuard::~ScopedAuthGuard() {
    if (m_auth) {
        m_auth->auditRecord(m_action, m_perm, m_granted);
    }
}

// ── 用户管理（切片 31，FR-AUTH-01 完整版）──

namespace {
/// 随机 salt（两个 64 位随机数 hex 拼接，32 hex 字符）
QString randomSalt() {
    return QString::fromLatin1(
        QByteArray::number(QRandomGenerator::global()->generate64(), 16) +
        QByteArray::number(QRandomGenerator::global()->generate64(), 16));
}
/// 生成 sha256$<salt>$<hash> 存储串
QString hashPasswordWithSalt(const QString& password, const QString& salt) {
    return QStringLiteral("sha256$%1$%2").arg(salt, sha256Hex(salt.toUtf8() + password.toUtf8()));
}
}  // namespace

QVector<AuthManager::UserInfo> AuthManager::listUsers() const {
    QVector<UserInfo> out;
    out.reserve(m_users.size());
    for (const UserRec& u : m_users) {
        out.push_back(UserInfo{u.username, u.role, u.mustChange});
    }
    return out;
}

bool AuthManager::setMustChange(const QString& username, bool must) {
    UserRec* u = const_cast<UserRec*>(findUser(username));
    if (!u) return false;
    u->mustChange = must;
    appendAudit(QStringLiteral("admin"), QStringLiteral("user.mustchange"),
                username, true);
    return true;
}

bool AuthManager::requiresPasswordChange() const {
    if (!m_loggedIn) return false;
    const UserRec* u = findUser(m_currentUser);
    return u != nullptr && u->mustChange;
}

bool AuthManager::addUser(const QString& username, const QString& password, UserRole role) {
    if (username.isEmpty() || password.isEmpty()) return false;
    if (findUser(username) != nullptr) return false;   // 已存在
    UserRec rec;
    rec.username = username;
    rec.password = hashPasswordWithSalt(password, randomSalt());
    rec.role     = role;
    m_users.push_back(rec);
    appendAudit(QStringLiteral("admin"), QStringLiteral("user.add"), username, true);
    return true;
}

bool AuthManager::removeUser(const QString& username) {
    if (username == m_currentUser) return false;   // 禁删自己（防自杀锁死）
    const auto it = std::find_if(m_users.begin(), m_users.end(),
        [&](const UserRec& u) { return u.username == username; });
    if (it == m_users.end()) return false;
    m_users.erase(it);
    appendAudit(QStringLiteral("admin"), QStringLiteral("user.remove"), username, true);
    return true;
}

bool AuthManager::changePassword(const QString& username, const QString& newPassword) {
    if (newPassword.isEmpty()) return false;
    UserRec* u = const_cast<UserRec*>(findUser(username));
    if (!u) return false;
    u->password = hashPasswordWithSalt(newPassword, randomSalt());
    u->mustChange = false;   // 切片 32：改密即清除首登强改密标志
    appendAudit(username, QStringLiteral("user.chpwd"), {}, true);
    return true;
}

bool AuthManager::saveUsersToJson(const QString& path) {
    QFileInfo fi(path);
    const QDir dir = fi.absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) return false;

    QJsonArray arr;
    for (const UserRec& u : m_users) {
        QJsonObject o;
        o.insert(QStringLiteral("username"), u.username);
        o.insert(QStringLiteral("password"), u.password);   // 已哈希
        o.insert(QStringLiteral("role"), u.role == UserRole::Admin   ? QStringLiteral("Admin")
                                          : u.role == UserRole::Engineer ? QStringLiteral("Engineer")
                                                                         : QStringLiteral("Operator"));
        o.insert(QStringLiteral("mustChange"), u.mustChange);   // 切片 32
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("users"), arr);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    appendAudit(QStringLiteral("admin"), QStringLiteral("user.save"), path, true);
    return true;
}

}  // namespace ens::business
