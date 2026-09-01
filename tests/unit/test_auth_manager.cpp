// tests/unit/test_auth_manager.cpp —— AuthManager 最小认证单测（切片 19）。
// 覆盖：默认用户回退 / users.json 加载 / 登录成败 / 角色 / 锁定解锁 / 空闲计时。
// ⚠ TEST_CASE 第一参数严格 ASCII（项目测试铁律，中文放注释）。
#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>
#include <QFile>

#include "business/AuthManager.h"

using ens::business::AuthManager;
using ens::business::UserRole;

namespace {

// 写一个含 3 用户的 users.json（含一个非法空用户名条目验证跳过）
void writeUsersJson(const QString& path) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(R"({
      "users": [
        {"username": "admin",    "password": "a123",  "role": "Admin"},
        {"username": "eng",      "password": "e123",  "role": "engineer"},
        {"username": "op",       "password": "o123",  "role": "Operator"},
        {"username": "",         "password": "x",     "role": "Admin"}
      ]
    })");
}

}  // namespace

TEST_CASE("auth: missing users.json falls back to builtin defaults") {
    AuthManager auth;
    // 路径不存在 → 回退内置默认（admin/operator），返回 false
    REQUIRE_FALSE(auth.loadUsersFromJson(QStringLiteral("Z:/no_such/users.json")));
    REQUIRE(auth.userCount() >= 2);
}

TEST_CASE("auth: load users.json accepts roles case-insensitively") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("users.json"));
    writeUsersJson(path);

    AuthManager auth;
    REQUIRE(auth.loadUsersFromJson(path));
    REQUIRE(auth.userCount() == 3);   // 空用户名条目被跳过
}

TEST_CASE("auth: login ok with correct credential and role mapping") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("users.json"));
    writeUsersJson(path);

    AuthManager auth;
    auth.loadUsersFromJson(path);

    REQUIRE(auth.login(QStringLiteral("admin"), QStringLiteral("a123")));
    REQUIRE(auth.isLoggedIn());
    REQUIRE(auth.currentUser() == QStringLiteral("admin"));
    REQUIRE(auth.currentRole() == UserRole::Admin);
}

TEST_CASE("auth: login rejected on bad user or bad password") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("users.json"));
    writeUsersJson(path);

    AuthManager auth;
    auth.loadUsersFromJson(path);

    REQUIRE_FALSE(auth.login(QStringLiteral("nobody"), QStringLiteral("a123")));
    REQUIRE_FALSE(auth.login(QStringLiteral("admin"), QStringLiteral("wrong")));
    REQUIRE_FALSE(auth.isLoggedIn());
}

TEST_CASE("auth: logout clears session") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("users.json"));
    writeUsersJson(path);

    AuthManager auth;
    auth.loadUsersFromJson(path);
    auth.login(QStringLiteral("eng"), QStringLiteral("e123"));

    auth.logout();
    REQUIRE_FALSE(auth.isLoggedIn());
    REQUIRE(auth.currentUser().isEmpty());
}

TEST_CASE("auth: lock and unlock with current user password") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("users.json"));
    writeUsersJson(path);

    AuthManager auth;
    auth.loadUsersFromJson(path);
    auth.login(QStringLiteral("op"), QStringLiteral("o123"));

    auth.lock();
    REQUIRE(auth.isLocked());
    // 锁定期间不允许再登录（防止绕过锁屏）
    REQUIRE_FALSE(auth.login(QStringLiteral("op"), QStringLiteral("o123")));
    // 错误密码不能解锁
    REQUIRE_FALSE(auth.unlock(QStringLiteral("bad")));
    REQUIRE(auth.isLocked());
    // 正确密码解锁
    REQUIRE(auth.unlock(QStringLiteral("o123")));
    REQUIRE_FALSE(auth.isLocked());
}

TEST_CASE("auth: idle timer resets on login and touchActivity") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("users.json"));
    writeUsersJson(path);

    AuthManager auth;
    auth.loadUsersFromJson(path);
    auth.login(QStringLiteral("admin"), QStringLiteral("a123"));

    // 未登录不计时；刚登录 idle 接近 0
    REQUIRE(auth.idleSeconds() < 2);

    // touchActivity 重置计时
    auth.touchActivity();
    REQUIRE(auth.idleSeconds() < 2);
}
