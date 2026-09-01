// apps/device_simulator/src/main_gui.cpp
// 测试台 CLI 入口（切片 13：SimulatorEngine 编排者落地；切片 15：--scenario/--export-dir）。
// 默认 5 秒自动停（4.3.4 联调场景可由 main 程序拉起测试台进程）。
// 完整 GUI 控制台（设备树/寄存器表/故障面板/场景运行器）属 B10（Phase 4 起步切片 17）。
#include "sim/SimulatorEngine.h"
#include "sim/sim_config.h"

#include <QApplication>
#include <QTimer>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

// 极简 CLI 解析：--key value / 位置参数 runSec。未知 --key 报错退出。
int parseCli(int argc, char* argv[], std::string* scenario, std::string* exportDir,
             std::string* pointtable, int* runSec) noexcept {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--scenario") {
            if (i + 1 >= argc) return -1;
            *scenario = argv[++i];
        } else if (a == "--export-dir") {
            if (i + 1 >= argc) return -1;
            *exportDir = argv[++i];
        } else if (a == "--pointtable") {
            if (i + 1 >= argc) return -1;
            *pointtable = argv[++i];
        } else if (a == "--help" || a == "-h") {
            return 1;
        } else {
            // 位置参数 = 运行秒数（默认 5）
            *runSec = std::atoi(a.c_str());
            if (*runSec <= 0) return -1;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    int runSec = 5;
    std::string scenarioPath;
    std::string exportDir;
    std::string pointtablePath = "docs/04-测试台/data/sim_pointtable_sample.json";
    const int rc = parseCli(argc, argv, &scenarioPath, &exportDir, &pointtablePath, &runSec);
    if (rc == 1) {
        std::cout << "usage: DeviceSimulator [runSec] [--scenario <path>] [--export-dir <dir>]\n"
                  << "       [--pointtable <path>]\n";
        return 0;
    }
    if (rc != 0) {
        std::cerr << "bad arguments (try --help)\n";
        return 2;
    }

    ens::sim::SimulatorEngine engine;
    // 默认配置:tickMs=100, 23 从站拓扑由 fromPointTable 推导,TCP 让 OS 分配端口
    ens::sim::SimConfig cfg;
    cfg.tickMs = 100;
    cfg.seed   = 0;
    cfg.tcp.port = 0;  // OS 分配,实际端口由 emu->tcpPort() 读出
    // CLI 模式默认纯 TCP:RTU 打开依赖 com0com 虚拟串口,未就绪会让整体启动失败
    // （emu.start 的 DoD 严格语义）。B10 GUI 控制台按 FR-SIM-09 默认双链路启用。
    cfg.rtu.enabled = false;
    cfg.pointtablePath = pointtablePath;
    cfg.scenarioPath = scenarioPath;
    cfg.exportDir    = exportDir;

    if (!engine.start(cfg)) {
        std::cerr << "[main] SimulatorEngine start failed\n";
        return 1;
    }

    // QTimer 定时停 + 退出事件循环（B10 之前用此模式让 4.3.4 联调拉起测试台进程）
    QTimer::singleShot(runSec * 1000, &app, [&engine, runSec]() {
        std::cout << "[main] CLI timeout " << runSec
                  << "s, stopping engine (tickCount=" << engine.tickCount() << ")\n";
        // stop() 内部 finishReport + 落盘 + 打印 result（场景报告在 stop 前不可用）
        engine.stop();
        QApplication::quit();
    });

    std::cout << "[main] DeviceSimulator CLI running for " << runSec
              << "s" << (scenarioPath.empty() ? "" : " scenario=" + scenarioPath)
              << " (Ctrl+C to abort)\n";
    return app.exec();
}
