// tests/unit/test_alarm_word_io.cpp
// 测试台 ── 告警字 IO 编码（切片 15 B8 收尾 / ENS-SIM-IMP §3.1）。
//
// 用例覆盖：
//   1) OverTemp 注入 → alarmWord bit0 → holding[AlarmWord 槽位] 可见（IO 编码链路）
//   2) CellVoltage ± → bit1/bit2 → holding 可见
//   3) abort 后复位：alarmWord 清零 + holding 清零（每 tick 先清后置语义）
//   4) RECOVER 回归到 recoverValue(35) 后复位（overheat_drill 70s 复位验收）

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include "core/point_table.h"
#include "sim/FaultInjector.h"
#include "sim/point_generator.h"
#include "sim/sim_config.h"

using namespace ens::sim;
using Catch::Approx;

namespace {
std::shared_ptr<SimPointTable> makeLoadedTable() {
    static const std::filesystem::path candidates[] = {
        std::filesystem::path{L"test_data/sim_pointtable_sample.json"},
        std::filesystem::path{L"../test_data/sim_pointtable_sample.json"},
        std::filesystem::path{L"../../bin/Debug/test_data/sim_pointtable_sample.json"},
    };
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            return SimPointTable::loadFromJsonFile(p);
        }
    }
    throw std::runtime_error("alarm_word_io test: sample JSON not found");
}

// Rack-01 告警字槽位（点表按名字查,不硬码）
uint16_t alarmWordAddrOf(const SimPointTable& pt, const char* name) {
    const SimPoint* p = pt.findByName(name);
    REQUIRE(p != nullptr);
    return p->registerAddr;
}

// 注入 + 单 tick 助手
struct Rig {
    std::shared_ptr<SimPointTable> pt;   // 先声明,保证 gen 构造时 pt 存活
    PointGenerator gen;
    FaultInjector  fi;
    uint16_t       alarmAddr;
    Rig(std::shared_ptr<SimPointTable> p, SimConfig cfg)
        : pt(p), gen(cfg, p), alarmAddr(alarmWordAddrOf(*pt, "Rack-01_AlarmWord")) {
        gen.attachFi(&fi);
    }
};

SimConfig makeCfg() {
    SimConfig cfg;
    cfg.tickMs = 100;
    cfg.seed   = 0;
    return cfg;
}
}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// 1) OverTemp → bit0 → holding[AlarmWord]
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("alarm_word_io: OverTemp sets bit0 visible in holding[AlarmWord] (B8 closeout)",
          "[master][sim][alarmword][io][tier2]") {
    auto pt = makeLoadedTable();
    Rig rig(pt, makeCfg());

    FaultRequest req;
    req.spec.type        = FaultType::OverTemp;
    req.spec.scope       = Scope::POINT;
    req.spec.slave       = 1;
    req.spec.reg         = 4096;   // Rack-01_MaxTemp
    req.spec.targetValue = 65.0f;
    rig.fi.trigger(req);

    rig.gen.generateTick();

    const auto& work = rig.gen.workOf(1);
    REQUIRE((work.alarmWord & 0x01u) != 0);
    REQUIRE((work.getHolding(rig.alarmAddr) & 0x01u) != 0);   // IO 编码可见
}

// ═════════════════════════════════════════════════════════════════════════════
// 2) CellVoltage ± → bit1/bit2 → holding 可见
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("alarm_word_io: CellVoltage +/- sets bit1/bit2 in holding[AlarmWord]",
          "[master][sim][alarmword][io][cellvoltage][tier2]") {
    auto pt = makeLoadedTable();
    // 过压 → bit1
    {
        Rig rig(pt, makeCfg());
        FaultRequest req;
        req.spec.type        = FaultType::CellVoltage;
        req.spec.scope       = Scope::POINT;
        req.spec.slave       = 1;
        req.spec.reg         = 4096;
        req.spec.targetValue = 4.5f;
        rig.fi.trigger(req);
        rig.gen.generateTick();
        const auto& work = rig.gen.workOf(1);
        REQUIRE((work.alarmWord & 0x02u) != 0);
        REQUIRE((work.getHolding(rig.alarmAddr) & 0x02u) != 0);
    }
    // 欠压 → bit2
    {
        Rig rig(pt, makeCfg());
        FaultRequest req;
        req.spec.type        = FaultType::CellVoltage;
        req.spec.scope       = Scope::POINT;
        req.spec.slave       = 1;
        req.spec.reg         = 4096;
        req.spec.targetValue = -0.5f;
        rig.fi.trigger(req);
        rig.gen.generateTick();
        const auto& work = rig.gen.workOf(1);
        REQUIRE((work.alarmWord & 0x04u) != 0);
        REQUIRE((work.getHolding(rig.alarmAddr) & 0x04u) != 0);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// 3) abort 后复位（每 tick 先清后置 → 无活跃覆盖即归零）
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("alarm_word_io: abort clears alarmWord and holding[AlarmWord] on next tick",
          "[master][sim][alarmword][io][abort][tier2]") {
    auto pt = makeLoadedTable();
    Rig rig(pt, makeCfg());

    FaultRequest req;
    req.spec.type        = FaultType::OverTemp;
    req.spec.scope       = Scope::POINT;
    req.spec.slave       = 1;
    req.spec.reg         = 4096;
    req.spec.targetValue = 65.0f;
    const FaultHandle h = rig.fi.trigger(req);
    REQUIRE(h != INVALID_FAULT_HANDLE);
    rig.gen.generateTick();
    REQUIRE((rig.gen.workOf(1).getHolding(rig.alarmAddr) & 0x01u) != 0);

    REQUIRE(rig.fi.abort(h));
    rig.gen.generateTick();   // 清零 + 无覆盖 → 归零
    const auto& work = rig.gen.workOf(1);
    REQUIRE(work.alarmWord == 0);
    REQUIRE(work.getHolding(rig.alarmAddr) == 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// 4) RECOVER 回归到 recoverValue(35) 后复位（overheat_drill 70s 验收语义）
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("alarm_word_io: recover(h,35) regresses then clears alarmWord (overheat semantics)",
          "[master][sim][alarmword][io][recover][tier2]") {
    auto pt = makeLoadedTable();
    Rig rig(pt, makeCfg());

    FaultRequest req;
    req.spec.type        = FaultType::OverTemp;
    req.spec.scope       = Scope::POINT;
    req.spec.slave       = 1;
    req.spec.reg         = 4096;
    req.spec.targetValue = 65.0f;
    req.spec.rampRate    = 100.0f;   // 快回归（65→35 约 300ms）
    const FaultHandle h = rig.fi.trigger(req);
    rig.gen.generateTick();
    REQUIRE((rig.gen.workOf(1).getHolding(rig.alarmAddr) & 0x01u) != 0);

    REQUIRE(rig.fi.recover(h, 35.0f));   // 回归到 35℃
    // RECOVERING 期:值开始回落但告警字仍置位
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    rig.fi.tickSessions(0);
    rig.gen.generateTick();
    {
        const auto& work = rig.gen.workOf(1);
        REQUIRE((work.alarmWord & 0x01u) != 0);   // 未到 IDLE,仍置位
        REQUIRE(rig.fi.sessionState(h) == FaultState::RECOVERING);
    }
    // 回归到位 → IDLE → 归零
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    rig.fi.tickSessions(0);
    REQUIRE(rig.fi.sessionState(h) == FaultState::IDLE);
    rig.gen.generateTick();
    {
        const auto& work = rig.gen.workOf(1);
        REQUIRE(work.alarmWord == 0);
        REQUIRE(work.getHolding(rig.alarmAddr) == 0);
    }
}
