// src/app/main.cpp —— EnerSentry 主程序入口（切片 19：GUI 双模式）。
//
// 形态：QApplication（GUI 默认 / --cli 保留原无窗口模式）。
// GUI 流程：AuthManager → 加载暗色主题 → LoginDialog（exec 成功）→ EnerSentryApp
//           → MainWindow（依赖注入 DataBus/AlarmEngine/AuthManager）。
// CLI 流程（--cli，4.3.4 联调资产）：参数解析 + 启动 + console 日志 + 优雅退出。
//
// 接线逻辑全部收敛在 EnerSentryApp（src/app/EnerSentryApp.h/.cpp）；
// UI 层仅依赖注入抽象（ens::ui 不 include 本文件 / app 层）。

#include "EnerSentryApp.h"

#include "auth/LoginDialog.h"
#include "common/theme.h"
#include "main/MainWindow.h"

#include "AuthManager.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QTimer>

#include <cstdio>

namespace {

// GUI 模式默认用户表路径（缺失时 AuthManager 回退内置 admin/operator 并告警）
const char* kDefaultUsersPath = "config/users.json";

}  // namespace

int main(int argc, char* argv[]) {
    // High DPI（5.1.4 起步：必须在 QApplication 构造前设置，HLD-UI §4.4）
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication app(argc, argv);
    QApplication::setApplicationName("ens_app");
    QApplication::setApplicationVersion("0.19.0");
    QApplication::setOrganizationName("EnerSentry");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "EnerSentry host application (GUI default; --cli for headless)");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption cliOpt("cli", "headless mode (4.3.4 联调 / 集成测试用)");
    QCommandLineOption hostOpt("host", "simulator TCP host", "host", "127.0.0.1");
    QCommandLineOption portOpt("port", "simulator TCP port", "port", "5020");
    QCommandLineOption ptOpt("point-table", "point table JSON path (required)", "path");
    QCommandLineOption pollOpt("poll-ms", "poll interval in ms", "ms", "100");
    QCommandLineOption runOpt("run-seconds",
                              "auto quit after N seconds (0 = run forever)", "sec", "0");
    QCommandLineOption rulesOpt("alarm-rules",
                                "alarm rules JSON path (empty = no rules)", "path");
    QCommandLineOption dataOpt("data-dir",
                               "monthly history DB root dir (empty = no persistence)", "dir");
    QCommandLineOption bboxOpt("blackbox-dir",
                               "blackbox critical mmap dir (empty = counting only)", "dir");
    QCommandLineOption sboOpt("cmd",
                              "one-shot SBO cmd: select:slave:reg:value[:e] | operate | cancel",
                              "cmd");
    QCommandLineOption usersOpt("users", "users.json path", "path", kDefaultUsersPath);
    parser.addOption(cliOpt);
    parser.addOption(hostOpt);
    parser.addOption(portOpt);
    parser.addOption(ptOpt);
    parser.addOption(pollOpt);
    parser.addOption(runOpt);
    parser.addOption(rulesOpt);
    parser.addOption(dataOpt);
    parser.addOption(bboxOpt);
    parser.addOption(sboOpt);
    parser.addOption(usersOpt);
    parser.process(app);

    const bool cliMode = parser.isSet(cliOpt);

    ens::app::EnerSentryApp::Options opts;
    opts.host            = parser.value(hostOpt);
    opts.port            = static_cast<quint16>(parser.value(portOpt).toUInt());
    opts.pointTablePath  = parser.value(ptOpt);
    opts.pollIntervalMs  = parser.value(pollOpt).toInt();
    opts.runSeconds      = parser.value(runOpt).toInt();
    opts.alarmRulesPath  = parser.value(rulesOpt);
    opts.dataDir         = parser.value(dataOpt);
    opts.blackboxDir     = parser.value(bboxOpt);
    opts.sboCmd          = parser.value(sboOpt);
    if (opts.pointTablePath.isEmpty()) {
        std::fprintf(stderr, "[ENS] usage: --point-table <json> is required\n");
        return 2;
    }

    // ───────────────────────── CLI 模式（原行为，保留联调资产）─────────────────────────
    if (cliMode) {
        ens::app::EnerSentryApp es(opts);
        QObject::connect(&es, &ens::app::EnerSentryApp::sampleReady,
                         [](uint32_t pid, qint64, double v) {
            std::printf("[ENS] pt=%u value=%.2f\n", pid, v);
        });
        QObject::connect(&es, &ens::app::EnerSentryApp::alarmRaised,
                         [](const QString& text) {
            std::printf("[ENS] %s\n", qPrintable(text));
        });
        if (!es.start()) return 1;
        if (opts.runSeconds > 0) {
            QTimer::singleShot(opts.runSeconds * 1000, &app, &QCoreApplication::quit);
        }
        const int rc = app.exec();
        es.stop();
        return rc;
    }

    // ───────────────────────── GUI 模式（切片 19）─────────────────────────
    // 1) 认证（FR-AUTH-01：登录成功才进主窗）
    ens::business::AuthManager auth;
    auth.loadUsersFromJson(parser.value(usersOpt));

    // 2) 暗色主题（SRS UI-01）
    ens::ui::applyTheme(&app);

    // 3) 登录首屏
    ens::ui::LoginDialog dlg(&auth);
    if (dlg.exec() != QDialog::Accepted) {
        return 0;   // 取消登录直接退出
    }

    // 4) 业务内核（采集/数据/告警管线）
    ens::app::EnerSentryApp es(opts);
    if (!es.start()) {
        QMessageBox::critical(nullptr, QStringLiteral("EnerSentry"),
                              QStringLiteral("通信内核启动失败，请检查点表路径与端口。"));
        return 1;
    }

    // 5) 主窗口（UiDeps 依赖注入，ens::ui 不触碰 app 层头）
    ens::ui::UiDeps deps;
    deps.bus   = es.dataBus();
    deps.alarm = es.alarmEngine();
    deps.auth  = &auth;
    deps.sbo   = es.sboStateMachine();
    deps.sboSelect = [&es](const ens::business::SboSelectRequest& req) {
        return es.submitSboSelect(req.slaveId, req.registerAddr, req.value, req.emergency);
    };
    deps.sboOperate = [&es](const QString& seq) { return es.submitSboOperate(seq); };
    deps.sboCancel  = [&es](const QString& seq) { return es.submitSboCancel(seq); };
    // 切片 23：Diag/Config 数据源
    deps.channel = es.channel();
    deps.pointTable = es.pointTable();
    deps.alarmRulesPath = es.alarmRulesPath();
    deps.alarmRuleCount = static_cast<int>(es.alarmEngine()->ruleCount());
    deps.host = opts.host;
    deps.port = opts.port;
    deps.pollMs = opts.pollIntervalMs;
    deps.linkLabel = QStringLiteral("%1:%2").arg(opts.host).arg(opts.port);

    ens::ui::MainWindow w(deps);
    // 链路状态 → 状态栏（切片 22，5C 联调：断链红/恢复绿）
    QObject::connect(&es, &ens::app::EnerSentryApp::connected, &w, [&w] {
        w.setLinkConnected(true);
    });
    QObject::connect(&es, &ens::app::EnerSentryApp::disconnected, &w, [&w] {
        w.setLinkConnected(false);
    });
    w.setLinkConnected(es.isConnected());
    w.show();

    const int rc = app.exec();
    es.stop();
    return rc;
}
