// tests/unit/test_fault_injector.cpp
// 测试台 ── FaultInjector Tier 2 单测（DevGuide §4B B7 / ENS-LLD-SIM §4.3-§4.5）。
//
// 用例覆盖（与方案评审 R8 一致）：
//   1) OverrideTable RCU 替换可见（trigger 后 resolveOverride 立即可见）
//   2) FaultSession IDLE→ACTIVE（trigger 后 state=ACTIVE）
//   3) ACTIVE→RECOVERING（durationMs 到期，sleep+tick 推进）
//   4) RECOVERING→IDLE（ramp 到位，sleep+tick 推进）
//   5) ACTIVE→ABORTED（abort 调用后 override 清除）
//   6) PointGenerator 集成：OverTemp trigger → generateTick → m_work 寄存器值上升

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using Catch::Approx;

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include "core/point_table.h"
#include "sim/FaultInjector.h"
#include "sim/point_generator.h"
#include "sim/sim_config.h"

using namespace ens::sim;

namespace {
// 与 test_point_generator.cpp 相同的点表加载助手
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
    throw std::runtime_error("FaultInjector test: sample JSON not found");
}
}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// 1) OverrideTable RCU 替换可见
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("fault_injector: trigger makes resolveOverride immediately visible (RCU snapshot)",
          "[master][sim][fault_injector][rcu][tier2]") {
    FaultInjector fi;

    FaultRequest req;
    req.spec.type      = FaultType::OverTemp;
    req.spec.scope     = Scope::POINT;
    req.spec.slave     = 3;
    req.spec.reg       = 0x205;
    req.spec.targetValue = 65.0f;
    req.spec.durationMs  = 0;  // 永久 ACTIVE（外部必须显式 abort）

    const FaultHandle h = fi.trigger(req);
    REQUIRE(h != INVALID_FAULT_HANDLE);
    REQUIRE(fi.sessionState(h) == FaultState::ACTIVE);
    REQUIRE(fi.sessionCount() == 1);
    REQUIRE(fi.tableSize() == 1);

    // 热路径：resolveOverride 立即可见
    const FaultEffect ef = fi.resolveOverride(3, 0x205);
    REQUIRE(ef.active);
    REQUIRE(ef.value == Approx(65.0f));
    REQUIRE(ef.type  == FaultType::OverTemp);
    REQUIRE_FALSE(ef.dropLink);
    REQUIRE_FALSE(ef.corruptCrc);

    // 其他 (slave,reg) 不应被覆盖
    const FaultEffect ef2 = fi.resolveOverride(3, 0x999);
    REQUIRE_FALSE(ef2.active);
    const FaultEffect ef3 = fi.resolveOverride(7, 0x205);
    REQUIRE_FALSE(ef3.active);
}

// ═════════════════════════════════════════════════════════════════════════════
// 2) FSM IDLE → ACTIVE
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("fault_injector: trigger transitions IDLE to ACTIVE; effect.type propagated",
          "[master][sim][fault_injector][fsm][tier2]") {
    FaultInjector fi;
    REQUIRE(fi.sessionCount() == 0);

    FaultRequest req;
    req.spec.type      = FaultType::CellVoltage;
    req.spec.scope     = Scope::POINT;
    req.spec.slave     = 1;
    req.spec.reg       = 0x10;
    req.spec.targetValue = 4.5f;

    const FaultHandle h = fi.trigger(req);
    REQUIRE(fi.sessionCount() == 1);
    REQUIRE(fi.sessionState(h) == FaultState::ACTIVE);

    const FaultEffect ef = fi.resolveOverride(1, 0x10);
    REQUIRE(ef.active);
    REQUIRE(ef.value == Approx(4.5f));
    REQUIRE(ef.type  == FaultType::CellVoltage);

    // 五类故障 type 字段正确性
    FaultRequest req2;
    req2.spec.type      = FaultType::CommLoss;
    req2.spec.scope     = Scope::POINT;
    req2.spec.slave     = 2;
    req2.spec.reg       = 0x10;
    const FaultHandle h2 = fi.trigger(req2);
    const FaultEffect ef2 = fi.resolveOverride(2, 0x10);
    REQUIRE(ef2.active);
    REQUIRE(ef2.dropLink);
    REQUIRE(ef2.type == FaultType::CommLoss);
}

// ═════════════════════════════════════════════════════════════════════════════
// 3) FSM ACTIVE → RECOVERING（durationMs 到期）
//    设计要点：RECOVERING 首次 tick 时 m_recoverSinceMs 刚被设，dt≈0 不会推进 ramp。
//    拆两步：第一次 tick 验证状态机转移 + effect 仍 active；等时间后第二次 tick 让 ramp 跑
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("fault_injector: ACTIVE transitions to RECOVERING; effect stays active during ramp",
          "[master][sim][fault_injector][fsm][recovering][tier2]") {
    FaultInjector fi;
    FaultRequest req;
    req.spec.type        = FaultType::OverTemp;
    req.spec.scope       = Scope::POINT;
    req.spec.slave       = 3;
    req.spec.reg         = 0x205;
    req.spec.targetValue = 65.0f;
    req.spec.durationMs  = 80;
    req.spec.rampRate    = 650.0f;  // 0.15s 内回归 97.5 > 65

    const FaultHandle h = fi.trigger(req);
    REQUIRE(fi.sessionState(h) == FaultState::ACTIVE);

    // 第一次 tick:ACTIVE→RECOVERING（durationMs 已过）
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    fi.tickSessions(120);
    REQUIRE(fi.sessionState(h) == FaultState::RECOVERING);

    // RECOVERING 期间 effect 仍 active
    const FaultEffect ef = fi.resolveOverride(3, 0x205);
    REQUIRE(ef.active);
    REQUIRE(ef.type == FaultType::OverTemp);
    // ramp 刚启动 dt≈0,value 仍接近 target
    REQUIRE(ef.value >= 0.0f);

    // 第二次 tick:让 ramp 跑动（rampRate=650 × 0.15s = 97.5 > 65）
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    fi.tickSessions(150);
    REQUIRE(fi.sessionState(h) == FaultState::IDLE);

    const FaultEffect ef2 = fi.resolveOverride(3, 0x205);
    REQUIRE_FALSE(ef2.active);
    REQUIRE(fi.tableSize() == 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// 4) FSM RECOVERING → IDLE（ramp 到位）
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("fault_injector: RECOVERING transitions to IDLE after ramp completes",
          "[master][sim][fault_injector][fsm][ramp][tier2]") {
    FaultInjector fi;
    FaultRequest req;
    req.spec.type        = FaultType::OverTemp;
    req.spec.scope       = Scope::POINT;
    req.spec.slave       = 3;
    req.spec.reg         = 0x205;
    req.spec.targetValue = 65.0f;
    req.spec.durationMs  = 50;
    req.spec.rampRate    = 650.0f;  // 0.1s 内回归 65

    const FaultHandle h = fi.trigger(req);

    // 触发 RECOVERING
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    fi.tickSessions(80);
    REQUIRE(fi.sessionState(h) == FaultState::RECOVERING);

    // 等 ramp 完成：650 × 0.2s = 130 > 65，1.5 × duration 足够
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    fi.tickSessions(200);
    REQUIRE(fi.sessionState(h) == FaultState::IDLE);

    // IDLE 后 resolveOverride 返回 active=false
    const FaultEffect ef = fi.resolveOverride(3, 0x205);
    REQUIRE_FALSE(ef.active);
    REQUIRE(fi.tableSize() == 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// 5) abort: ACTIVE → ABORTED；override 立即清除
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("fault_injector: abort transitions ACTIVE to ABORTED and clears override",
          "[master][sim][fault_injector][fsm][abort][tier2]") {
    FaultInjector fi;
    FaultRequest req;
    req.spec.type      = FaultType::CommLoss;
    req.spec.scope     = Scope::POINT;
    req.spec.slave     = 5;
    req.spec.reg       = 0x10;
    req.spec.targetValue = 1.0f;

    const FaultHandle h = fi.trigger(req);
    REQUIRE(fi.sessionState(h) == FaultState::ACTIVE);
    REQUIRE(fi.resolveOverride(5, 0x10).active);

    REQUIRE(fi.abort(h));
    REQUIRE(fi.sessionState(h) == FaultState::ABORTED);

    const FaultEffect ef = fi.resolveOverride(5, 0x10);
    REQUIRE_FALSE(ef.active);
    REQUIRE(fi.tableSize() == 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// 6) PointGenerator 集成：OverTemp trigger → generateTick → m_work 寄存器值覆盖
//    注:sim_pointtable_sample.json 里 Rack-01_MaxTemp 的 registerAddr=4096 (0x1000, BMS 簇 1 base+0x00)
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("fault_injector: PointGenerator integration: OverTemp overrides m_work holding register",
          "[master][sim][fault_injector][integration][point_generator][tier2]") {
    auto pt = makeLoadedTable();
    SimConfig cfg;
    cfg.tickMs = 100;
    cfg.seed   = 0;
    PointGenerator gen(cfg, pt);

    FaultInjector fi;
    gen.attachFi(&fi);

    // 触发 OverTemp on slave 1, register 4096 (Rack-01_MaxTemp, scale 0.1)
    FaultRequest req;
    req.spec.type        = FaultType::OverTemp;
    req.spec.scope       = Scope::POINT;
    req.spec.slave       = 1;
    req.spec.reg         = 4096;       // 0x1000 = BMS cluster 1 base+0x00
    req.spec.targetValue = 65.0f;      // 65℃
    req.spec.durationMs  = 0;
    const FaultHandle h = fi.trigger(req);
    REQUIRE(h != INVALID_FAULT_HANDLE);

    gen.generateTick();

    // PointGenerator 把 effect.value (65.0) 经 (eng - offset) / scale 转 raw
    // raw = (65.0 - 0) / 0.1 = 650,写入 m_work[1].holding[4096]
    const auto& work = gen.workOf(1);
    REQUIRE(work.getHolding(4096) == 650);
}

// ═════════════════════════════════════════════════════════════════════════════
// 7) linkEffect：B8 IO 层链路级故障查询（dropLink / corruptCrc / delayMs）
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("fault_injector: linkEffect returns SLAVE-scope flags (dropLink/delayMs) for IO layer",
          "[master][sim][fault_injector][linkeffect][io][tier2]") {
    FaultInjector fi;

    // 触发 CommLoss scope=SLAVE slave=3,不应有 raw 值,只产 IO 标志
    FaultRequest req;
    req.spec.type      = FaultType::CommLoss;
    req.spec.scope     = Scope::SLAVE;
    req.spec.slave     = 3;
    req.spec.targetValue = 1.0f;
    fi.trigger(req);

    // linkEffect(slave=3) 应返回 active=true + dropLink=true
    const FaultEffect ef = fi.linkEffect(3);
    REQUIRE(ef.active);
    REQUIRE(ef.dropLink);
    REQUIRE(ef.type == FaultType::CommLoss);

    // 链路 effect 对 POINT scope 注入不命中（POINT 写精确 key,不在 ALL_KEY 通配上）
    FaultRequest req2;
    req2.spec.type      = FaultType::CrcError;
    req2.spec.scope     = Scope::POINT;
    req2.spec.slave     = 5;
    req2.spec.reg       = 100;
    fi.trigger(req2);
    // linkEffect(5) 应返回 POINT scope CrcError 的 effect(IO 层忽略 reg)
    const FaultEffect ef5 = fi.linkEffect(5);
    REQUIRE(ef5.active);
    REQUIRE(ef5.corruptCrc);
    REQUIRE(ef5.corruptByte);
    REQUIRE(ef5.type == FaultType::CrcError);
    REQUIRE_FALSE(ef5.dropLink);   // 不是 CommLoss

    // Timeout 注入应返回 delayMs
    FaultRequest req3;
    req3.spec.type      = FaultType::Timeout;
    req3.spec.scope     = Scope::POINT;
    req3.spec.slave     = 7;
    req3.spec.reg       = 200;
    req3.spec.corruptMs = 500;
    fi.trigger(req3);
    const FaultEffect ef7 = fi.linkEffect(7);
    REQUIRE(ef7.active);
    REQUIRE(ef7.delayMs == 500);
}

// ═════════════════════════════════════════════════════════════════════════════
// 8) PointGenerator 告警字置位：OverTemp → bit0（B7 尾巴,B8 落地）
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("fault_injector: PointGenerator sets alarmWord bit0 on OverTemp (B8 deliverable)",
          "[master][sim][fault_injector][alarmword][overtemp][tier2]") {
    auto pt = makeLoadedTable();
    SimConfig cfg;
    cfg.tickMs = 100;
    cfg.seed   = 0;
    PointGenerator gen(cfg, pt);
    FaultInjector fi;
    gen.attachFi(&fi);

    // OverTemp on slave 1 (Rack-01)
    FaultRequest req;
    req.spec.type        = FaultType::OverTemp;
    req.spec.scope       = Scope::POINT;
    req.spec.slave       = 1;
    req.spec.reg         = 4096;       // Rack-01_MaxTemp
    req.spec.targetValue = 65.0f;
    fi.trigger(req);

    gen.generateTick();

    const auto& work = gen.workOf(1);
    // bit0 = OverTemp
    REQUIRE((work.alarmWord & 0x01u) != 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// 9) PointGenerator 告警字置位：CellVoltage value>0 → bit1,value<0 → bit2
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("fault_injector: PointGenerator sets alarmWord bit1/bit2 on CellVoltage (B8 deliverable)",
          "[master][sim][fault_injector][alarmword][cellvoltage][tier2]") {
    auto pt = makeLoadedTable();
    SimConfig cfg;
    cfg.tickMs = 100;
    cfg.seed   = 0;

    // 第一次 generateTick:OverVolt
    {
        PointGenerator gen(cfg, pt);
        FaultInjector fi;
        gen.attachFi(&fi);
        FaultRequest req;
        req.spec.type        = FaultType::CellVoltage;
        req.spec.scope       = Scope::POINT;
        req.spec.slave       = 1;
        req.spec.reg         = 4096;
        req.spec.targetValue = 4.5f;  // 过压
        fi.trigger(req);
        gen.generateTick();
        const auto& work = gen.workOf(1);
        REQUIRE((work.alarmWord & 0x02u) != 0);  // bit1 = OverVolt
    }
    // 第二次 generateTick:UnderVolt
    {
        PointGenerator gen(cfg, pt);
        FaultInjector fi;
        gen.attachFi(&fi);
        FaultRequest req;
        req.spec.type        = FaultType::CellVoltage;
        req.spec.scope       = Scope::POINT;
        req.spec.slave       = 1;
        req.spec.reg         = 4096;
        req.spec.targetValue = -0.5f;  // 欠压
        fi.trigger(req);
        gen.generateTick();
        const auto& work = gen.workOf(1);
        REQUIRE((work.alarmWord & 0x04u) != 0);  // bit2 = UnderVolt
    }
}
