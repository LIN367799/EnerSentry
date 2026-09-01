// tests/unit/test_auth_security.cpp —— NFR-SEC-06 安全加固单测（切片 28）。
// 覆盖：内置用户哈希登录 / 明文兼容（旧 users.json）/ 错误密码拒绝 / 5 次失败锁定 + 剩余秒 /
//       成功登录清计数 / unlock 走哈希校验。
// ⚠ TEST_CASE 第一参数严格 ASCII（项目测试铁律）。
#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>
#include <QFile>

#include <memory>

#include "AuthManager.h"

using ens::business::AuthManager;

namespace {
std::unique_ptr<AuthManager> makeAuth() {
    auto a = std::make_unique<AuthManager>();
    a->loadUsersFromJson(QString());   // 空路径 → 回退内置默认（哈希用户）
    return a;
}
// 写一份含明文密码的旧式 users.json（兼容性验证）
std::unique_ptr<AuthManager> makePlainAuth(QTemporaryDir& dir) {
    QFile f(dir.filePath(QStringLiteral("users.json")));
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(R"({"users":[{"username":"legacy","password":"plain123","role":"Engineer"}]})");
    f.close();
    auto a = std::make_unique<AuthManager>();
    a->loadUsersFromJson(dir.filePath(QStringLiteral("users.json")));
    return a;
}
}  // namespace

TEST_CASE("auth_security: hashed builtin users still log in") {
    auto a = makeAuth();
    REQUIRE(a->login(QStringLiteral("admin"), QStringLiteral("Admin@123")));
    REQUIRE(a->login(QStringLiteral("operator"), QStringLiteral("Operator@123")));
    REQUIRE_FALSE(a->login(QStringLiteral("admin"), QStringLiteral("wrong")));
}

TEST_CASE("auth_security: plaintext users.json remains compatible") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto a = makePlainAuth(dir);
    REQUIRE(a->login(QStringLiteral("legacy"), QStringLiteral("plain123")));
    REQUIRE(a->currentRole() == ens::business::UserRole::Engineer);
}

TEST_CASE("auth_security: five failures lock the username") {
    auto a = makeAuth();
    for (int i = 0; i < AuthManager::kMaxLoginFails; ++i) {
        REQUIRE_FALSE(a->login(QStringLiteral("admin"), QStringLiteral("bad")));
    }
    // 第 5 次失败已触发锁定 → 即使正确密码也被拒
    REQUIRE_FALSE(a->login(QStringLiteral("admin"), QStringLiteral("Admin@123")));
    REQUIRE(a->lockRemainingSeconds(QStringLiteral("admin")) > 0);
    // 其他用户名不受影响
    REQUIRE(a->login(QStringLiteral("operator"), QStringLiteral("Operator@123")));
}

TEST_CASE("auth_security: successful login clears failure count") {
    auto a = makeAuth();
    REQUIRE_FALSE(a->login(QStringLiteral("admin"), QStringLiteral("bad")));
    REQUIRE_FALSE(a->login(QStringLiteral("admin"), QStringLiteral("bad")));
    REQUIRE(a->login(QStringLiteral("admin"), QStringLiteral("Admin@123")));   // 成功清计数
    a->logout();
    // 之后继续 4 次失败不应触发锁定（计数已清零，需 5 次）
    for (int i = 0; i < AuthManager::kMaxLoginFails - 1; ++i) {
        REQUIRE_FALSE(a->login(QStringLiteral("admin"), QStringLiteral("bad")));
    }
    REQUIRE(a->lockRemainingSeconds(QStringLiteral("admin")) == 0);
    REQUIRE(a->login(QStringLiteral("admin"), QStringLiteral("Admin@123")));
}

TEST_CASE("auth_security: unlock verifies via hash") {
    auto a = makeAuth();
    REQUIRE(a->login(QStringLiteral("admin"), QStringLiteral("Admin@123")));
    a->lock();
    REQUIRE(a->isLocked());
    REQUIRE_FALSE(a->unlock(QStringLiteral("wrong")));
    REQUIRE(a->unlock(QStringLiteral("Admin@123")));
    REQUIRE_FALSE(a->isLocked());
}
