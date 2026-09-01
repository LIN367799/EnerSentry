// src/app/main.cpp —— EnerSentry 主程序入口（切片 14：CLI 最小可运行）。
//
// 形态：QCoreApplication（无 GUI）。启动流程：
//   ens_app --point-table <json> [--host 127.0.0.1] [--port 5020]
//           [--run-seconds 0] [--poll-ms 100]
//
// 接线逻辑全部收敛在 EnerSentryApp（src/app/EnerSentryApp.h/.cpp），
// 本文件仅做参数解析 + 启动 + console 日志 + 优雅退出。
// Phase 4 换 GUI 主窗时，本文件替换为 QApplication + 登录对话框 + MainWindow，
// EnerSentryApp 作为纯数据/业务内核保持不变。

#include "EnerSentryApp.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTimer>

#include <cstdio>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("ens_app");
    QCoreApplication::setApplicationVersion("0.14.0");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "EnerSentry host application (CLI, slice 14: minimal runnable)");
    parser.addHelpOption();
    parser.addVersionOption();

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
    parser.addOption(hostOpt);
    parser.addOption(portOpt);
    parser.addOption(ptOpt);
    parser.addOption(pollOpt);
    parser.addOption(runOpt);
    parser.addOption(rulesOpt);
    parser.addOption(dataOpt);
    parser.addOption(bboxOpt);
    parser.addOption(sboOpt);
    parser.process(app);

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
    es.stop();          // 优雅停（事件循环退出后）
    return rc;
}
