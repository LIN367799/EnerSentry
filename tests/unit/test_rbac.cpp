// tests/unit/test_rbac.cpp —— AuthManager RBAC 单测（切片 26）。
// 覆盖：权限矩阵（三级角色）/ 未登录与锁定拒绝 / 审计记录（登录、权限拒绝、显式）/ 环形截断 / ScopedAuthGuard。
// ⚠ TEST_CASE 第一参数严格 ASCII（项目测试铁律）。
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "AuthManager.h"

using ens::business::AuthManager;
using ens::business::UserRole;
using ens::business::ScopedAuthGuard;
namespace perms = ens::business::perms;   // 命名空间别名（全限定仍经 perms::kXxx）

namespace {
// 内置默认用户表：admin(Admin)/operator(Operator)。AuthManager 不可拷贝（QObject）→ 返回指针
std::unique_ptr<AuthManager> makeAuth() {
    auto a = std::make_unique<AuthManager>();
    a->loadUsersFromJson(QString());   // 空路径 → 回退内置默认
    return a;
}
}  // namespace

TEST_CASE("rbac: admin has all permissions") {
    auto a = makeAuth();
    REQUIRE(a->login(QStringLiteral("admin"), QStringLiteral("Admin@123")));
    REQUIRE(a->currentRole() == UserRole::Admin);
    REQUIRE(a->checkPermission(perms::kSboOperate));
    REQUIRE(a->checkPermission(perms::kConfigView));
    REQUIRE(a->checkPermission(perms::kAuthManage));
    REQUIRE(a->checkPermission(perms::kSystemExit));
}

TEST_CASE("rbac: operator is read-only") {
    auto a = makeAuth();
    REQUIRE(a->login(QStringLiteral("operator"), QStringLiteral("Operator@123")));
    REQUIRE(a->currentRole() == UserRole::Operator);
    REQUIRE_FALSE(a->checkPermission(perms::kSboOperate));
    REQUIRE_FALSE(a->checkPermission(perms::kConfigView));
    REQUIRE(a->checkPermission(perms::kSystemExit));     // 只读 + 退出允许
    REQUIRE(a->checkPermission(QStringLiteral("history.view")));
}

TEST_CASE("rbac: denied before login or while locked") {
    auto a = makeAuth();
    REQUIRE_FALSE(a->checkPermission(perms::kSboOperate));   // 未登录
    a->login(QStringLiteral("admin"), QStringLiteral("Admin@123"));
    a->lock();
    REQUIRE_FALSE(a->checkPermission(perms::kSboOperate));   // 已锁定
}

TEST_CASE("rbac: audit records login and permission denial") {
    auto b = makeAuth();
    b->login(QStringLiteral("operator"), QStringLiteral("Operator@123"));
    b->checkPermission(perms::kSboOperate);                       // denied → 审计
    const auto log = b->auditLog();
    bool sawLogin = false, sawDenied = false;
    for (const auto& e : log) {
        if (e.action == QStringLiteral("login") && e.granted) sawLogin = true;
        if (e.action == QStringLiteral("perm:sbo.operate") && !e.granted) sawDenied = true;
    }
    REQUIRE(sawLogin);
    REQUIRE(sawDenied);
}

TEST_CASE("rbac: audit ring buffer caps at limit") {
    auto a = makeAuth();
    a->login(QStringLiteral("admin"), QStringLiteral("Admin@123"));
    for (int i = 0; i < AuthManager::kAuditCap + 50; ++i) {
        a->auditRecord(QStringLiteral("tick"), QString::number(i));
    }
    REQUIRE(a->auditLog().size() <= static_cast<size_t>(AuthManager::kAuditCap));
}

TEST_CASE("rbac: ScopedAuthGuard grants and audits") {
    auto a = makeAuth();
    a->login(QStringLiteral("operator"), QStringLiteral("Operator@123"));
    {
        ScopedAuthGuard g(a.get(), perms::kSboOperate, QStringLiteral("sbo.operate"));
        REQUIRE_FALSE(g.granted());   // Operator 无 SBO 权限
    }
    const auto log = a->recentAudit();
    REQUIRE_FALSE(log.empty());
    REQUIRE(log.back().action == QStringLiteral("sbo.operate"));
    REQUIRE_FALSE(log.back().granted);
}
