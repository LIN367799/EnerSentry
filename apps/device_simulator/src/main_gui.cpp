// apps/device_simulator/src/main_gui.cpp
// 测试台入口（切片 18：B10 双模式）。
//   * 默认 GUI：SimulatorMainWindow 控制台（寄存器监视/故障注入/场景运行/事件日志）。
//   * --cli：保留 CLI 模式（切片 13/15 联调资产依赖：run_drill_434.ps1、4.3.4 端到端）。
#include "sim/SimulatorEngine.h"
#include "sim/sim_config.h"
#include "gui/main_window.h"

#include <QApplication>
#include <QTimer>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

// 极简 CLI 解析：--key value / 位置参数 runSec / --cli。未知 --key 报错退出。
// 返回：0=正常，1=打印 usage，-1=参数错误。
int parseCli(int argc, char* argv[], bool* cliMode, std::string* scenario,
             std::string* exportDir, std::string* pointtable, int* tcpPort,
             int* runSec) noexcept {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--cli") {
            *cliMode = true;
        } else if (a == "--scenario") {
            if (i + 1 >= argc) return -1;
            *scenario = argv[++i];
        } else if (a == "--export-dir") {
            if (i + 1 >= argc) return -1;
            *exportDir = argv[++i];
        } else if (a == "--pointtable") {
            if (i + 1 >= argc) return -1;
            *pointtable = argv[++i];
        } else if (a == "--port") {
            if (i + 1 >= argc) return -1;
            *tcpPort = std::atoi(argv[++i]);
        } else if (a == "--help" || a == "-h") {
            return 1;
        } else {
            // 位置参数 = 运行秒数（默认 5，仅 CLI 模式有效）
            *runSec = std::atoi(a.c_str());
            if (*runSec <= 0) return -1;
        }
    }
    return 0;
}

// 公共配置装配（GUI/CLI 共用默认：纯 TCP，RTU 需 com0com 手动在 GUI 勾选）
ens::sim::SimConfig makeDefaultCfg(int tcpPort, const std::string& pointtablePath,
                                   const std::string& scenarioPath,
                                   const std::string& exportDir) {
    ens::sim::SimConfig cfg;
    cfg.tickMs = 100;
    cfg.seed   = 0;
    cfg.tcp.port = static_cast<uint16_t>(tcpPort);  // CLI 传 0 = OS 分配；GUI 默认 5020
    // 默认纯 TCP：RTU 打开依赖 com0com 虚拟串口，未就绪会让整体启动失败（DoD 严格语义）。
    // GUI 模式可在「运行配置」面板勾选「启用 RTU」后应用重启。
    cfg.rtu.enabled = false;
    cfg.pointtablePath = pointtablePath;
    cfg.scenarioPath = scenarioPath;
    cfg.exportDir    = exportDir;
    return cfg;
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    bool cliMode = false;
    int runSec = 5;
    int tcpPort = 0;   // CLI:0 = OS 分配；GUI:0 将落到 SimConfig 默认 5020
    std::string scenarioPath;
    std::string exportDir;
    // 切片 44a：数据工件迁至仓根 data/（原 docs/04-测试台/data/），默认路径全 ASCII
    std::string pointtablePath = "data/sim_pointtable_sample.json";

    const int rc = parseCli(argc, argv, &cliMode, &scenarioPath, &exportDir,
                            &pointtablePath, &tcpPort, &runSec);
    if (rc == 1) {
        std::cout << "usage: DeviceSimulator [--cli] [runSec] [--scenario <path>]\n"
                  << "       [--export-dir <dir>] [--pointtable <path>] [--port <tcpPort>]\n"
                  << "       （默认 GUI 控制台；--cli 进入原 CLI 模式，供联调脚本/自动化）\n";
        return 0;
    }
    if (rc != 0) {
        std::cerr << "bad arguments (try --help)\n";
        return 2;
    }

    // ───────────────────────── CLI 模式（原行为，保留联调资产）─────────────────────────
    if (cliMode) {
        ens::sim::SimulatorEngine engine;
        const auto cfg = makeDefaultCfg(tcpPort, pointtablePath, scenarioPath, exportDir);
        if (!engine.start(cfg)) {
            std::cerr << "[main] SimulatorEngine start failed\n";
            return 1;
        }
        QTimer::singleShot(runSec * 1000, &app, [&engine, runSec]() {
            std::cout << "[main] CLI timeout " << runSec
                      << "s, stopping engine (tickCount=" << engine.tickCount() << ")\n";
            engine.stop();   // 内部 finishReport + 落盘
            QApplication::quit();
        });
        std::cout << "[main] DeviceSimulator CLI running for " << runSec
                  << "s" << (scenarioPath.empty() ? "" : " scenario=" + scenarioPath)
                  << " (Ctrl+C to abort)\n";
        return app.exec();
    }

    // ───────────────────────── GUI 模式（B10 控制台）─────────────────────────
    ens::sim::SimulatorEngine engine;
    // GUI 默认固定 5020（SIM-IMP §10 验收标准：状态栏显示 127.0.0.1:5020）
    auto cfg = makeDefaultCfg(tcpPort == 0 ? 5020 : tcpPort, pointtablePath,
                              scenarioPath, exportDir);

    // SimulatorMainWindow 构造时自动启动引擎；失败弹 critical 不退出（可改配置重试）
    SimulatorMainWindow w(&engine, cfg);
    w.show();
    return app.exec();
}
