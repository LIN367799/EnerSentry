// tests/unit/test_ui_smoke.cpp —— GUI 冒烟（切片 33：QApplication/offscreen 自动化）。
// 替代手工 GUI 冒烟：验证 .ui 编译产物 / qrc 图标 / 对话框与主窗构造链不崩。
// 覆盖：LoginDialog 构造+显示 / AuditLogDialog + UserManagerDialog 构造 /
//       MainWindow 全视图实例化（真实 auth/alarm/sbo/bus + null 只读数据源）。
// ⚠ TEST_CASE 第一参数严格 ASCII（项目测试铁律）。
#include <catch2/catch_test_macros.hpp>

#include <QApplication>

#include "auth/LoginDialog.h"
#include "controls/AuditLogDialog.h"
#include "controls/CriticalAlarmDialog.h"
#include "controls/UserManagerDialog.h"
#include "main/MainWindow.h"

#include "AlarmEngine.h"
#include "AuthManager.h"
#include "DataBus.h"
#include "SboStateMachine.h"

namespace {
void pumpEvents(int ms = 100) {
    QApplication::processEvents(QEventLoop::AllEvents, ms);
}
}  // namespace

TEST_CASE("ui_smoke: login dialog constructs, shows and closes",
          "[ui][smoke][tier2]") {
    ens::business::AuthManager auth;
    auth.loadUsersFromJson(QString());
    ens::ui::LoginDialog dlg(&auth);
    dlg.show();
    pumpEvents();
    REQUIRE(dlg.isVisible());   // offscreen 下 show 成功
    dlg.close();
    pumpEvents();
}

TEST_CASE("ui_smoke: audit and user manager dialogs construct",
          "[ui][smoke][tier2]") {
    ens::business::AuthManager auth;
    auth.loadUsersFromJson(QString());
    auth.login(QStringLiteral("admin"), QStringLiteral("Admin@123"));

    ens::ui::AuditLogDialog audit(&auth);
    audit.show();
    pumpEvents();
    audit.close();

    ens::ui::UserManagerDialog um(&auth, QStringLiteral("users_out.json"));
    um.show();
    pumpEvents();
    um.close();
    pumpEvents();
}

TEST_CASE("ui_smoke: critical alarm dialog constructs, shows and closes",
          "[ui][smoke][tier2]") {
    ens::business::AlarmEvent ev;
    ev.id = 1; ev.pointId = 7; ev.level = ens::business::AlarmLevel::Critical;
    ev.triggerTime = 0; ev.alarmValue = 66.5; ev.threshold = 50.0;
    ev.description = "point=7 value=66.5 threshold=50";

    ens::ui::CriticalAlarmDialog dlg(ev, QStringLiteral("Rack-01_MaxTemp"));
    dlg.show();
    pumpEvents();
    REQUIRE(dlg.isVisible());
    dlg.close();
    pumpEvents();
}

TEST_CASE("ui_smoke: main window instantiates all seven views",
          "[ui][smoke][tier2]") {
    // 真实可构造对象（无外部依赖）；只读数据源（channel/pointTable/dal）留空
    ens::business::AuthManager auth;
    auth.loadUsersFromJson(QString());
    auth.login(QStringLiteral("admin"), QStringLiteral("Admin@123"));
    ens::datahub::DataBus bus;
    ens::business::AlarmEngine alarm;
    ens::business::SboStateMachine sbo;

    ens::ui::UiDeps deps;
    deps.bus   = &bus;
    deps.alarm = &alarm;
    deps.auth  = &auth;
    deps.sbo   = &sbo;
    deps.host  = QStringLiteral("127.0.0.1");
    deps.port  = 5020;
    deps.pollMs = 1000;
    deps.usersPath = QStringLiteral("users_out.json");

    ens::ui::MainWindow w(deps);
    w.show();
    pumpEvents();
    REQUIRE(w.isVisible());
    w.close();
    pumpEvents();
}
