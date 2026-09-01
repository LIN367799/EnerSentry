// src/business/AuthManager.cpp —— 切片 19 最小认证实现（ENS-LLD-400 §4 子集）。
#include "AuthManager.h"

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
    if (!u || u->password != password) return false;
    m_currentUser = username;
    m_currentRole = u->role;
    m_loggedIn = true;
    m_locked = false;
    m_idle.restart();
    emit loginChanged(m_currentUser, static_cast<int>(m_currentRole));
    return true;
}

void AuthManager::logout() {
    m_loggedIn = false;
    m_locked = false;
    m_currentUser.clear();
    m_currentRole = UserRole::Operator;
    emit loggedOut();
}

void AuthManager::lock() {
    if (!m_loggedIn) return;
    m_locked = true;
    emit lockedChanged(true);
}

bool AuthManager::unlock(const QString& password) {
    if (!m_loggedIn || !m_locked) return false;
    const UserRec* u = findUser(m_currentUser);
    if (!u || u->password != password) return false;
    m_locked = false;
    m_idle.restart();
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

}  // namespace ens::business
