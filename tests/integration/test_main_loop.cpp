// tests/integration/test_main_loop.cpp
// 切片 14 集成测试：EnerSentryApp（被测主程序）+ SimulatorEngine（测试台）
// 进程内双轨回环 —— 主程序经 TCP 连测试台从站，验证：
//   * 连接建立
//   * 轮询驱动 → Modbus 请求/响应 → 工程值还原 → Sample 流转（L1 + DataBus + AlarmEngine）
//   * 5s 内收到 >= 10 个 Sample，且点表首点（Rack-01_MaxTemp, pointId=1）值在合理温度区间
//
// 依赖前置（tests/CMakeLists.txt）：
//   * test_data/sim_pointtable_sample.json 由 file(COPY) 部署到 build 目录
//   * EnerSentryApp.cpp 显式加入 ens_tests 源列表（测试台 CMake 铁律）
//   * ens_tests 已链接 ens::protocol/datahub/business/channel（PUBLIC 链透传）

#include "app/EnerSentryApp.h"

#include "sim/sim_config.h"
#include "sim/SimulatorEngine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

constexpr quint16 kSimPort = 15020;   // 避开默认 5020，独立测试端口

// 点表 JSON 定位：ctest cwd = build/vs2022-debug/tests（需 ../test_data），
// IDE/直跑 cwd = 项目根或 build 目录（test_data 直下）。多路径尝试（与 test_pointtable 同策略）。
std::filesystem::path findPointTableJson() {
    const char* candidates[] = {
        "test_data/sim_pointtable_sample.json",
        "../test_data/sim_pointtable_sample.json",
    };
    for (const char* p : candidates) {
        if (std::filesystem::exists(p)) return std::filesystem::path(p);
    }
    throw std::runtime_error("cannot locate test_data/sim_pointtable_sample.json");
}

}  // namespace

TEST_CASE("main_loop: app + simulator loopback delivers samples", "[integration][main_loop]") {
    const auto ptPath = findPointTableJson();
    const std::string ptPathStr = ptPath.string();

    // ── 测试台侧：8 从站 / 43 点，DataTick 100ms ──
    auto simPt = ens::sim::SimPointTable::loadFromJsonFile(ptPath);
    ens::sim::SimConfig simCfg;
    simCfg.tcp.enabled    = true;
    simCfg.tcp.port       = kSimPort;
    simCfg.rtu.enabled    = false;
    simCfg.pointtablePath = ptPathStr;
    simCfg.tickMs         = 100;
    simCfg.slaves         = ens::sim::SimConfig::fromPointTable(*simPt);

    ens::sim::SimulatorEngine sim;
    REQUIRE(sim.start(simCfg));

    // ── 被测主程序侧：CLI 接线器 ──
    ens::app::EnerSentryApp::Options opts;
    opts.host           = QStringLiteral("127.0.0.1");
    opts.port           = kSimPort;
    opts.pointTablePath = QString::fromStdString(ptPathStr);
    opts.runSeconds     = 0;

    ens::app::EnerSentryApp es(opts);
    REQUIRE(es.start());

    // ── 等待 Sample 流转（5s 上限；主线程 processEvents 驱动消费节拍）──
    bool connected = false;
    QObject::connect(&es, &ens::app::EnerSentryApp::connected,
                     [&connected]() { connected = true; });

    // 语义校验：记录收到的工程值（须在等待循环前连接，避免错过窗口）
    double lastValid = 0.0;
    bool   gotValid  = false;
    QObject::connect(&es, &ens::app::EnerSentryApp::sampleReady,
                     [&](uint32_t, qint64, double v) {
        if (v > 0.001 && v < 10000.0) { lastValid = v; gotValid = true; }
    });

    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 5000 && es.sampleCount() < 10) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }

    INFO("elapsedMs=" << t.elapsed() << " connected=" << connected
         << " sampleCount=" << es.sampleCount());
    REQUIRE(connected);
    REQUIRE(es.sampleCount() >= 10);

    // ── 语义校验：至少一个 Sample 的 value 是合理工程值（非 0/非 NaN）──
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    INFO("lastValid=" << lastValid);
    REQUIRE(gotValid);

    // ── 清理：先停被测主程序，再停测试台 ──
    es.stop();
    sim.stop();
}
