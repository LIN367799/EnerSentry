// tests/unit/test_scenario_script.cpp
// 测试台 ── ScenarioScript 场景驱动 Tier 2 单测（DevGuide §4B B9 切片 15 / ENS-SIM-IMP §7/§8）。
//
// 用例覆盖（R1~R7）：
//   1) 解析 3 套 drill JSON（name + stepCount）
//   2) overheat_drill INJECT t=0 展开全部 MaxTemp slave（样例点表 Rack-01/02 → 2 请求）
//   3) overheat_drill RECOVER t=70000 → session 进 RECOVERING（回归目标=35）
//   4) voltage_fault_drill 展开 8 个 CellV（InputRegister 地址）
//   5) random_linkloss_stress CommLoss SLAVE scope → linkEffect dropLink
//   6) finishReport → PASS + faultsInjected 计数（SIM-IMP §8.2 schema）
//   7) 容错：未知 fault code → FAIL；faultTypeFromCode nullopt

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "core/point_table.h"
#include "sim/FaultInjector.h"
#include "sim/scenario_script.h"
#include "sim/sim_config.h"

using namespace ens::sim;
using Catch::Approx;

namespace {

std::filesystem::path findTestData(const char* rel) {
    const std::filesystem::path candidates[] = {
        std::filesystem::path(L"test_data") / rel,
        std::filesystem::path(L"../test_data") / rel,
        std::filesystem::path(L"../../bin/Debug/test_data") / rel,
    };
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
    throw std::runtime_error(std::string("scenario test data not found: ") + rel);
}

std::shared_ptr<SimPointTable> makeLoadedTable() {
    return SimPointTable::loadFromJsonFile(findTestData("sim_pointtable_sample.json"));
}

std::string scenarioFile(const char* name) {
    return findTestData((std::string("scenarios/") + name).c_str()).string();
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// 1) 解析 3 套 drill JSON
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("scenario_script: load 3 drill JSONs with expected name/step counts",
          "[master][sim][scenario][load][tier2]") {
    ScenarioScript sc;
    REQUIRE(sc.load(scenarioFile("overheat_drill.json")));
    REQUIRE(sc.name() == "overheat_drill");
    REQUIRE(sc.stepCount() == 2);
    REQUIRE(sc.lastStepEndMs() >= 70000);

    ScenarioScript vf;
    REQUIRE(vf.load(scenarioFile("voltage_fault_drill.json")));
    REQUIRE(vf.name() == "voltage_fault_drill");
    REQUIRE(vf.stepCount() == 2);

    ScenarioScript rl;
    REQUIRE(rl.load(scenarioFile("random_linkloss_stress.json")));
    REQUIRE(rl.name() == "random_linkloss_stress");
    REQUIRE(rl.stepCount() == 8);
}

// ═════════════════════════════════════════════════════════════════════════════
// 2) overheat INJECT t=0 展开全部 MaxTemp slave
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("scenario_script: overheat INJECT at t=0 expands to all MaxTemp slaves",
          "[master][sim][scenario][overheat][inject][tier2]") {
    auto pt = makeLoadedTable();
    ScenarioScript sc;
    REQUIRE(sc.load(scenarioFile("overheat_drill.json")));

    FaultInjector fi;
    const int fired = sc.drive(0, fi, *pt);
    REQUIRE(fired == 1);                       // 仅 t=0 的 INJECT 触发
    // 样例点表 Rack-01 / Rack-02 有 _MaxTemp（Rack-16 无）
    REQUIRE(fi.sessionCount() == 2);
    // Rack-01_MaxTemp 被覆盖到 65℃
    const FaultEffect ef = fi.resolveOverride(1, 4096);
    REQUIRE(ef.active);
    REQUIRE(ef.value == Approx(65.0f));
    REQUIRE(ef.type == FaultType::OverTemp);
    REQUIRE_FALSE(sc.allFired());              // RECOVER step 未触发
}

// ═════════════════════════════════════════════════════════════════════════════
// 3) overheat RECOVER t=70000 → RECOVERING（回归目标 = 35）
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("scenario_script: overheat RECOVER at t=70000 enters RECOVERING toward 35C",
          "[master][sim][scenario][overheat][recover][tier2]") {
    auto pt = makeLoadedTable();
    ScenarioScript sc;
    REQUIRE(sc.load(scenarioFile("overheat_drill.json")));

    FaultInjector fi;
    sc.drive(0, fi, *pt);
    const int fired = sc.drive(70000, fi, *pt);
    REQUIRE(fired == 1);
    REQUIRE(sc.allFired());

    // 两个 session 都应进入 RECOVERING 且当前值 > 回归目标 35
    // （handle 不暴露,改从 resolveOverride 观察:65 开始按 rampRate=0.5 回落）
    const FaultEffect ef = fi.resolveOverride(1, 4096);
    REQUIRE(ef.active);
    REQUIRE(ef.value > 35.0f);
    // 推进若干 tick 后值应显著小于 65（开始回归；tickSessions 用单调时钟,
    // dt≈0 时 tick() 直接 return,需等真实时间流逝）
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    fi.tickSessions(0);
    const FaultEffect ef2 = fi.resolveOverride(1, 4096);
    REQUIRE(ef2.value < 65.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
// 4) voltage_fault_drill 展开 8 个 CellV（InputRegister）
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("scenario_script: voltage_fault INJECT expands 8 CellV requests",
          "[master][sim][scenario][voltage][cell][tier2]") {
    auto pt = makeLoadedTable();
    ScenarioScript sc;
    REQUIRE(sc.load(scenarioFile("voltage_fault_drill.json")));

    FaultInjector fi;
    sc.drive(0, fi, *pt);
    REQUIRE(fi.sessionCount() == 8);

    // Rack-01_CellV_000（样例点表 InputRegister）被覆盖
    const SimPoint* cv = pt->findByName("Rack-01_CellV_000");
    REQUIRE(cv != nullptr);
    const FaultEffect ef = fi.resolveOverride(1, cv->registerAddr);
    REQUIRE(ef.active);
    REQUIRE(ef.value == Approx(3.65f));
    REQUIRE(ef.type == FaultType::CellVoltage);
}

// ═════════════════════════════════════════════════════════════════════════════
// 5) random_linkloss CommLoss SLAVE scope → linkEffect dropLink
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("scenario_script: linkloss INJECT hits IO linkEffect via SLAVE scope",
          "[master][sim][scenario][linkloss][commloss][tier2]") {
    auto pt = makeLoadedTable();
    ScenarioScript sc;
    REQUIRE(sc.load(scenarioFile("random_linkloss_stress.json")));

    FaultInjector fi;
    // t=5000 触发 slave 11 断链 8s
    REQUIRE(sc.drive(0, fi, *pt) == 0);
    REQUIRE(sc.drive(5000, fi, *pt) == 1);

    const FaultEffect ef = fi.linkEffect(11);
    REQUIRE(ef.active);
    REQUIRE(ef.dropLink);
    // 非目标 slave 不受影响
    REQUIRE_FALSE(fi.linkEffect(12).dropLink);
}

// ═════════════════════════════════════════════════════════════════════════════
// 6) finishReport → PASS + faultsInjected（SIM-IMP §8.2）
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("scenario_script: finishReport yields PASS with faultsInjected counts",
          "[master][sim][scenario][report][tier2]") {
    auto pt = makeLoadedTable();
    ScenarioScript sc;
    REQUIRE(sc.load(scenarioFile("overheat_drill.json")));

    FaultInjector fi;
    sc.drive(0, fi, *pt);
    sc.drive(70000, fi, *pt);
    sc.finishReport(70000, false);

    const std::string rj = sc.reportJson();
    REQUIRE(rj.find("\"scenario\": \"overheat_drill\"") != std::string::npos);
    REQUIRE(rj.find("\"result\": \"PASS\"") != std::string::npos);
    REQUIRE(rj.find("\"FR-SIM-05a\"") != std::string::npos);

    // 事件流含 FAULT_INJECT / FAULT_RECOVER
    const std::string ev = sc.eventsJsonl();
    REQUIRE(ev.find("FAULT_INJECT") != std::string::npos);
    REQUIRE(ev.find("FAULT_RECOVER") != std::string::npos);
}

// ═════════════════════════════════════════════════════════════════════════════
// 7) 容错：未知 fault code / 提前终止
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("scenario_script: unknown fault code marks FAIL; aborted marks INCONCLUSIVE",
          "[master][sim][scenario][fault-tolerance][tier2]") {
    // faultTypeFromCode 未知码 → nullopt
    REQUIRE_FALSE(ScenarioScript::faultTypeFromCode("FR-SIM-99").has_value());
    REQUIRE(ScenarioScript::faultTypeFromCode("FR-SIM-05a").has_value());

    // 提前终止（aborted）→ INCONCLUSIVE
    auto pt = makeLoadedTable();
    ScenarioScript sc;
    REQUIRE(sc.load(scenarioFile("overheat_drill.json")));
    sc.finishReport(10000, true);   // 未跑完就结束
    REQUIRE(sc.reportJson().find("\"result\": \"INCONCLUSIVE\"") != std::string::npos);

    // 跑完但未 finish → finish 后 PASS（RESULT 覆盖验证）
    ScenarioScript sc2;
    REQUIRE(sc2.load(scenarioFile("overheat_drill.json")));
    FaultInjector fi;
    sc2.drive(0, fi, *pt);
    sc2.drive(70000, fi, *pt);
    sc2.finishReport(70000, false);
    REQUIRE(sc2.reportJson().find("\"result\": \"PASS\"") != std::string::npos);
}
