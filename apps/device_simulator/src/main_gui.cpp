// apps/device_simulator/src/main_gui.cpp
// 测试台 CLI 入口（切片 13：SimulatorEngine 编排者落地）
// 默认 5 秒自动停（4.3.4 联调场景可由 main 程序拉起测试台进程）。
// 完整 GUI 控制台（设备树/寄存器表/故障面板/场景运行器）属 B10（Phase 4 起步切片 17）。
#include "sim/SimulatorEngine.h"
#include "sim/sim_config.h"

#include <QApplication>
#include <QTimer>

#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // CLI 参数:argv[1] = 运行秒数（默认 5）
    int runSec = 5;
    if (argc > 1) runSec = std::atoi(argv[1]);

    ens::sim::SimulatorEngine engine;
    // 默认配置:tickMs=100, 23 从站拓扑由 fromPointTable 推导,TCP 让 OS 分配端口
    ens::sim::SimConfig cfg;
    cfg.tickMs = 100;
    cfg.seed   = 0;
    cfg.tcp.port = 0;  // OS 分配,实际端口由 emu->tcpPort() 读出

    if (!engine.start(cfg)) {
        std::cerr << "[main] SimulatorEngine start failed\n";
        return 1;
    }

    // QTimer 定时停 + 退出事件循环（B10 之前用此模式让 4.3.4 联调拉起测试台进程）
    QTimer::singleShot(runSec * 1000, &app, [&engine, runSec]() {
        std::cout << "[main] CLI timeout " << runSec
                  << "s, stopping engine (tickCount=" << engine.tickCount() << ")\n";
        engine.stop();
        QApplication::quit();
    });

    std::cout << "[main] DeviceSimulator CLI running for " << runSec
              << "s (Ctrl+C to abort)\n";
    return app.exec();
}
