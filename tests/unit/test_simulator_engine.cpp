// tests/unit/test_simulator_engine.cpp
// 测试台 ── SimulatorEngine 编排者 Tier 2 单测（DevGuide §4B 切片 13 / ENS-LLD-SIM §2.2.1）。
//
// 用例覆盖（与方案评审 R1~R6 一致）：
//   1) start/stop 生命周期：start 后 isRunning=true + tickCount>0,stop 后 isRunning=false
//   2) DataTick 驱动：start 后等 200ms,verify tickCount >= 2(100ms tick)
//   3) injectFault 基本调用：start 后 inject Fault,verify handle 非 0 + tableSize 增

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

#include "core/point_table.h"
#include "sim/FaultInjector.h"
#include "sim/sim_config.h"
#include "sim/SimulatorEngine.h"

using namespace ens::sim;

namespace {
SimConfig makeTestCfg() {
    static const std::filesystem::path candidates[] = {
        std::filesystem::path{L"test_data/sim_pointtable_sample.json"},
        std::filesystem::path{L"../test_data/sim_pointtable_sample.json"},
        std::filesystem::path{L"../../bin/Debug/test_data/sim_pointtable_sample.json"},
    };
    std::filesystem::path ptPath;
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) { ptPath = p; break; }
    }
    SimConfig cfg;
    cfg.tickMs          = 100;
    cfg.seed            = 0;
    cfg.pointtablePath  = ptPath.string();
    cfg.tcp.enabled     = false;  // 测试不真开 socket
    cfg.rtu.enabled     = false;
    return cfg;
}
}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// 1) start/stop 生命周期
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("simulator_engine: start/stop lifecycle transitions isRunning correctly",
          "[master][sim][simulator_engine][lifecycle][tier2]") {
    SimulatorEngine engine;
    REQUIRE_FALSE(engine.isRunning());
    REQUIRE(engine.tickCount() == 0);

    const auto cfg = makeTestCfg();
    REQUIRE(engine.start(cfg));
    REQUIRE(engine.isRunning());

    // 等几个 tick 后停
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const uint64_t tcBefore = engine.tickCount();
    REQUIRE(tcBefore > 0);

    engine.stop();
    REQUIRE_FALSE(engine.isRunning());
    // 停后 tickCount 冻结
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(engine.tickCount() == tcBefore);
}

// ═════════════════════════════════════════════════════════════════════════════
// 2) DataTick 线程驱动
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("simulator_engine: DataTick thread drives gen.generateTick at configured rate",
          "[master][sim][simulator_engine][datatick][tier2]") {
    SimulatorEngine engine;
    const auto cfg = makeTestCfg();
    REQUIRE(engine.start(cfg));

    // tickMs=100,等 250ms 应至少有 2 次 tick
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const uint64_t tc1 = engine.tickCount();
    REQUIRE(tc1 >= 2);

    // 再等 200ms 至少再 +1 tick
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint64_t tc2 = engine.tickCount();
    REQUIRE(tc2 > tc1);

    engine.stop();
}

// ═════════════════════════════════════════════════════════════════════════════
// 3) injectFault 线程安全基本调用
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("simulator_engine: injectFault basic call from caller thread returns non-zero handle",
          "[master][sim][simulator_engine][injectfault][tier2]") {
    SimulatorEngine engine;
    const auto cfg = makeTestCfg();
    REQUIRE(engine.start(cfg));

    // 等几 tick 让 emulator 准备好
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    FaultRequest req;
    req.spec.type        = FaultType::OverTemp;
    req.spec.scope       = Scope::POINT;
    req.spec.slave       = 1;
    req.spec.reg         = 4096;
    req.spec.targetValue = 65.0f;
    const FaultHandle h = engine.injectFault(req);
    REQUIRE(h != INVALID_FAULT_HANDLE);

    // 等一 tick 让覆盖写入 m_work（PointGenerator 在 tick 时调 resolveOverride）
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // 收回
    REQUIRE(engine.recoverFault(h));
    engine.stop();
}
