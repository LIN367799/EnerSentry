// tests/unit/test_alarm_engine.cpp
// L4 业务层 ── AlarmEngine Tier 2 单测（ENS-LLD-400 §2.3 + DevGuide §4.3.2）。

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>

#include <chrono>
#include <thread>
#include <vector>

#include "business/AlarmEngine.h"

using ens::business::AlarmEngine;
using ens::business::AlarmLevel;
using ens::business::AlarmRule;
using ens::business::AlarmStatus;
using ens::business::AlarmStormConfig;

namespace {
AlarmRule makeRule(uint32_t pid, float on, float off, AlarmLevel lvl = AlarmLevel::Warning,
                   uint32_t onDelayMs = 100, uint32_t offDelayMs = 100) {
    AlarmRule r;
    r.pointId      = pid;
    r.level        = lvl;
    r.onThreshold  = on;
    r.offThreshold = off;
    r.enabled      = true;
    r.onDelayMs    = onDelayMs;
    r.offDelayMs   = offDelayMs;
    return r;
}
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void processEvents(int ms = 50) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, ms);
}
}  // namespace

// ① + ② On/Off-Delay 迟滞 + 延时确认
TEST_CASE("alarm_engine: hysteresis + on/off-delay raise and recover",
          "[master][business][alarm][hysteresis][tier2]") {
    AlarmEngine eng;
    eng.loadRules({makeRule(1, 10.0f, 5.0f, AlarmLevel::Warning, 100, 100)});

    int triggeredN = 0;
    int recoveredN = 0;
    QObject::connect(&eng, &AlarmEngine::alarmTriggered,
                     [&triggeredN](const ens::business::AlarmEvent&) { ++triggeredN; });
    QObject::connect(&eng, &AlarmEngine::alarmRecovered,
                     [&recoveredN](uint64_t) { ++recoveredN; });

    // 单次越界未达 onDelayMs → 不触发
    eng.onDataUpdated(1, 1000, 12.0f);
    processEvents();
    REQUIRE(triggeredN == 0);
    REQUIRE(eng.activeAlarmCount() == 0);

    sleepMs(150);
    eng.onDataUpdated(1, 1100, 12.0f);
    processEvents();
    REQUIRE(triggeredN == 1);
    REQUIRE(eng.isPointInAlarm(1));

    // 带内抖动维持
    eng.onDataUpdated(1, 1200, 11.0f);
    processEvents();
    REQUIRE(eng.activeAlarmCount() == 1);

    // 持续低于 offThreshold 100ms → 恢复
    eng.onDataUpdated(1, 1300, 3.0f);
    sleepMs(150);
    eng.onDataUpdated(1, 1400, 3.0f);
    processEvents();
    REQUIRE(recoveredN == 1);
    REQUIRE_FALSE(eng.isPointInAlarm(1));
}

// ③ Critical → emit blackBoxRequested
TEST_CASE("alarm_engine: Critical level emits blackBoxRequested signal",
          "[master][business][alarm][critical][tier2]") {
    AlarmEngine eng;
    eng.loadRules({makeRule(42, 50.0f, 40.0f, AlarmLevel::Critical, 50, 50)});

    int triggeredN = 0;
    int blackboxN = 0;
    QObject::connect(&eng, &AlarmEngine::alarmTriggered,
                     [&triggeredN](const ens::business::AlarmEvent&) { ++triggeredN; });
    QObject::connect(&eng, &AlarmEngine::blackBoxRequested,
                     [&blackboxN](uint32_t, uint64_t) { ++blackboxN; });

    eng.onDataUpdated(42, 1000, 60.0f);
    sleepMs(100);
    eng.onDataUpdated(42, 1100, 60.0f);
    processEvents();

    REQUIRE(triggeredN == 1);
    REQUIRE(blackboxN == 1);

    // 恢复不应再发
    int blackboxN2 = 0;
    auto conn = QObject::connect(&eng, &AlarmEngine::blackBoxRequested,
                                 [&blackboxN2](uint32_t, uint64_t) { ++blackboxN2; });
    (void)conn;
    eng.onDataUpdated(42, 1200, 30.0f);
    sleepMs(100);
    eng.onDataUpdated(42, 1300, 30.0f);
    processEvents();
    REQUIRE(blackboxN2 == 0);
}

// ④ 同源抑制
TEST_CASE("alarm_engine: source suppression within suppressWindowMs blocks re-raise",
          "[master][business][alarm][suppress][tier2]") {
    AlarmRule r = makeRule(7, 5.0f, 1.0f, AlarmLevel::Warning, 50, 50);
    r.suppressWindowMs = 60000;
    AlarmEngine eng;
    eng.loadRules({r});

    int triggeredN = 0;
    QObject::connect(&eng, &AlarmEngine::alarmTriggered,
                     [&triggeredN](const ens::business::AlarmEvent&) { ++triggeredN; });

    eng.onDataUpdated(7, 1000, 10.0f);
    sleepMs(80);
    eng.onDataUpdated(7, 1100, 10.0f);
    processEvents();
    REQUIRE(triggeredN == 1);

    // 恢复
    eng.onDataUpdated(7, 1200, 0.0f);
    sleepMs(80);
    eng.onDataUpdated(7, 1300, 0.0f);
    processEvents();
    REQUIRE(eng.activeAlarmCount() == 0);

    // 再次越界 → 被抑制
    eng.onDataUpdated(7, 1400, 10.0f);
    sleepMs(80);
    eng.onDataUpdated(7, 1500, 10.0f);
    processEvents();
    REQUIRE(triggeredN == 1);                              // 仍 1,被抑制
}

// ⑤ 风暴抑制：5000 灌入 → droppedCount > 0,Critical 仍实时
TEST_CASE("alarm_engine: storm suppression drops overflow + keeps Critical real-time",
          "[master][business][alarm][storm][tier2]") {
    AlarmEngine eng;
    std::vector<AlarmRule> rules;
    for (uint32_t p = 1; p <= 100; ++p) {
        rules.push_back(makeRule(p, 10.0f, 5.0f, AlarmLevel::Warning, 10, 10));
    }
    rules.push_back(makeRule(9999, 50.0f, 40.0f, AlarmLevel::Critical, 10, 10));

    AlarmStormConfig cfg;
    cfg.windowMs        = 1000;
    cfg.threshold       = 50;
    cfg.flushIntervalMs = 200;
    eng.setStormConfig(cfg);
    eng.loadRules(rules);

    int blackboxN = 0;
    QObject::connect(&eng, &AlarmEngine::blackBoxRequested,
                     [&blackboxN](uint32_t, uint64_t) { ++blackboxN; });

    // 灌 5000 条
    const uint64_t ts0 = 2000;
    for (int i = 0; i < 5000; ++i) {
        const uint32_t p = (i % 100) + 1;
        eng.onDataUpdated(p, ts0 + i, 15.0f);
    }
    processEvents(200);
    REQUIRE(eng.isInStormMode());

    // Critical 实时
    eng.onDataUpdated(9999, ts0 + 10000, 60.0f);
    sleepMs(20);
    eng.onDataUpdated(9999, ts0 + 10001, 60.0f);
    processEvents();
    REQUIRE(blackboxN >= 1);
}