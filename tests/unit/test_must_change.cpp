// tests/unit/test_must_change.cpp —— 首登强改密单测（切片 32，FR-AUTH-01）。
// 覆盖：JSON mustChange 加载 / login 后 requiresPasswordChange / changePassword 清除 /
//       setMustChange / save+reload 保持标志 / 非标志用户不受影响。
// ⚠ TEST_CASE 第一参数严格 ASCII（项目测试铁律）。
#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>
#include <QFile>

#include <memory>

#include "AuthManager.h"

using ens::business::AuthManager;

namespace {
// 写一份含 mustChange 用户的 users.json
std::unique_ptr<AuthManager> makeMustChangeAuth(QTemporaryDir& dir) {
    QFile f(dir.filePath(QStringLiteral("users.json")));
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(R"({"users":[
        {"username":"admin","password":"sha256$ens-salt-v1$21888813a45eed2911e66baa92ca2cc6269ce425b8d3ff2b303ce139a03283e1","role":"Admin"},
        {"username":"tmp1","password":"sha256$ens-salt-v1$0acfaa9f0eac4258061f2521fabb91108bb97f07271d4acc2d256e16d4d03a3b","role":"Operator","mustChange":true}
    ]})");
    f.close();
    auto a = std::make_unique<AuthManager>();
    a->loadUsersFromJson(dir.filePath(QStringLiteral("users.json")));
    return a;
}
}  // namespace

TEST_CASE("must_change: flagged user requires password change after login") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto a = makeMustChangeAuth(dir);
    REQUIRE(a->login(QStringLiteral("tmp1"), QStringLiteral("Operator@123")));
    REQUIRE(a->requiresPasswordChange());     // 标志用户登录后需改密
    REQUIRE_FALSE(a->requiresPasswordChange() == false);
    // 改密 → 清除标志
    REQUIRE(a->changePassword(QStringLiteral("tmp1"), QStringLiteral("New@999")));
    REQUIRE_FALSE(a->requiresPasswordChange());
}

TEST_CASE("must_change: unflagged user unaffected") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto a = makeMustChangeAuth(dir);
    REQUIRE(a->login(QStringLiteral("admin"), QStringLiteral("Admin@123")));
    REQUIRE_FALSE(a->requiresPasswordChange());
}

TEST_CASE("must_change: setMustChange toggles and persists") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto a = makeMustChangeAuth(dir);
    // 给 admin 打标志
    REQUIRE(a->setMustChange(QStringLiteral("admin"), true));
    REQUIRE(a->login(QStringLiteral("admin"), QStringLiteral("Admin@123")));
    REQUIRE(a->requiresPasswordChange());
    // save + reload 保持
    const QString out = dir.filePath(QStringLiteral("out.json"));
    REQUIRE(a->saveUsersToJson(out));
    AuthManager b;
    REQUIRE(b.loadUsersFromJson(out));
    REQUIRE(b.login(QStringLiteral("admin"), QStringLiteral("Admin@123")));
    REQUIRE(b.requiresPasswordChange());
}
