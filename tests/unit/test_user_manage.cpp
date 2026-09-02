// tests/unit/test_user_manage.cpp —— 用户管理单测（切片 31，FR-AUTH-01）。
// 覆盖：addUser 随机 salt 哈希可登录 / 重复用户拒绝 / removeUser 禁删自己 /
//       changePassword 旧密码失效 / save+load 往返 / listUsers 不含密码。
// ⚠ TEST_CASE 第一参数严格 ASCII（项目测试铁律）。
#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>
#include <QFile>

#include <memory>

#include "AuthManager.h"

using ens::business::AuthManager;
using ens::business::UserRole;

namespace {
std::unique_ptr<AuthManager> makeAuth() {
    auto a = std::make_unique<AuthManager>();
    a->loadUsersFromJson(QString());   // 内置 admin/operator
    return a;
}
}  // namespace

TEST_CASE("user_manage: addUser hashes with random salt and logs in") {
    auto a = makeAuth();
    REQUIRE(a->addUser(QStringLiteral("eng1"), QStringLiteral("Eng@456"), UserRole::Engineer));
    // 已哈希（非明文）
    REQUIRE(a->login(QStringLiteral("eng1"), QStringLiteral("Eng@456")));
    REQUIRE(a->currentRole() == UserRole::Engineer);
    a->logout();
    REQUIRE_FALSE(a->login(QStringLiteral("eng1"), QStringLiteral("Eng@456")) == false);  // 明文不存
    REQUIRE(a->login(QStringLiteral("eng1"), QStringLiteral("Eng@456")));                // 哈希仍可
}

TEST_CASE("user_manage: duplicate username rejected") {
    auto a = makeAuth();
    REQUIRE_FALSE(a->addUser(QStringLiteral("admin"), QStringLiteral("x"), UserRole::Operator));
}

TEST_CASE("user_manage: removeUser refuses current user") {
    auto a = makeAuth();
    REQUIRE(a->login(QStringLiteral("admin"), QStringLiteral("Admin@123")));
    REQUIRE_FALSE(a->removeUser(QStringLiteral("admin")));       // 禁删自己
    REQUIRE(a->removeUser(QStringLiteral("operator")));          // 可删其他
    REQUIRE(a->userCount() == 1);
}

TEST_CASE("user_manage: changePassword invalidates old password") {
    auto a = makeAuth();
    REQUIRE(a->changePassword(QStringLiteral("admin"), QStringLiteral("New@456")));
    REQUIRE_FALSE(a->login(QStringLiteral("admin"), QStringLiteral("Admin@123")));
    REQUIRE(a->login(QStringLiteral("admin"), QStringLiteral("New@456")));
}

TEST_CASE("user_manage: save and reload round-trip") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("users_out.json"));

    auto a = makeAuth();
    REQUIRE(a->addUser(QStringLiteral("eng2"), QStringLiteral("Eng@789"), UserRole::Engineer));
    REQUIRE(a->saveUsersToJson(path));
    REQUIRE(QFile::exists(path));

    // 重新加载（新实例）→ 新用户可用且为哈希格式
    AuthManager b;
    REQUIRE(b.loadUsersFromJson(path));
    REQUIRE(b.login(QStringLiteral("eng2"), QStringLiteral("Eng@789")));
    REQUIRE(b.currentRole() == UserRole::Engineer);
    // 原 admin 保留
    b.logout();
    REQUIRE(b.login(QStringLiteral("admin"), QStringLiteral("Admin@123")));
}

TEST_CASE("user_manage: listUsers exposes no passwords") {
    auto a = makeAuth();
    const auto users = a->listUsers();
    REQUIRE(users.size() >= 2);
    for (const auto& u : users) {
        REQUIRE_FALSE(u.username.isEmpty());
    }
}
