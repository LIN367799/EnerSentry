// src/business/AuthManager.cpp —— 认证 + RBAC（切片 19 最小版 + 切片 26 V1.6：权限/审计/守卫）。
#include "AuthManager.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>
#include <initializer_list>
#include <tuple>

namespace ens::business {

namespace {

// 内置默认用户（users.json 缺失/空表时的开发期回退）
const std::initializer_list<std::tuple<const char*, const char*, const char*>>
    kDefaultUsers = {
        {"admin",    "Admin@123",    "Admin"},
        {"operator", "Operator@123", "Operator"},
};

UserRole roleFromString(const QString& s) {
    const QString low = s.trimmed().toLower();
    if (low == QStringLiteral("admin"))    return UserRole::Admin;
    if (low == QStringLiteral("engineer")) return UserRole::Engineer;
    return UserRole::Operator;
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
                rec.username = o.value(QStringLiteral("username")).toString();
                rec.password = o.value(QStringLiteral("password")).toString();
                rec.role     = roleFromString(o.value(QStringLiteral("role")).toString());
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
    const UserRec* u = findUser(username);
    if (!u || u->password != password) {
        appendAudit(username, QStringLiteral("login"), QStringLiteral("rejected"), false);
        return false;
    }
    m_currentUser = username;
    m_currentRole = u->role;
    m_loggedIn = true;
    m_locked = false;
    m_idle.restart();
    appendAudit(m_currentUser, QStringLiteral("login"), QStringLiteral("ok"), true);
    emit loginChanged(m_currentUser, static_cast<int>(m_currentRole));
    return true;
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
    if (!u || u->password != password) {
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

}  // namespace ens::business
