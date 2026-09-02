// tests/unit/test_alarm_notifier.cpp —— AlarmNotifier Tier 2 单测（切片 37，FR-AL-06）。
// 覆盖：① Critical → 通知一次（计数/信号/事件透传）
//       ② Warning/Info → 不通知
//       ③ 1s 防抖：窗口内多条 Critical 只通知首条；窗口过后恢复
//       ④ 风暴模式 → 抑制弹窗（计数）

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <vector>

#include <QDateTime>

#include "business/AlarmEngine.h"
#include "ui/common/AlarmNotifier.h"

using ens::business::AlarmEngine;
using ens::business::AlarmDirection;
using ens::business::AlarmEvent;
using ens::business::AlarmLevel;
using ens::business::AlarmRule;
using ens::business::AlarmStormConfig;
using ens::ui::AlarmNotifier;

namespace {

AlarmRule makeRule(uint32_t pid, float on, float off, AlarmLevel lvl) {
    AlarmRule r;
    r.pointId          = pid;
    r.level            = lvl;
    r.direction        = AlarmDirection::High;
    r.onThreshold      = on;
    r.offThreshold     = off;
    r.enabled          = true;
    r.onDelayMs        = 0;
    r.offDelayMs       = 0;
    r.suppressWindowMs = 0;
    return r;
}
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
uint64_t nowMs() { return static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()); }

}  // namespace

TEST_CASE("alarm notifier: critical triggers one notification", "[master][ui][alarm-notifier]") {
    AlarmEngine eng;
    AlarmNotifier notifier(&eng);

    uint64_t notifiedId = 0;
    QObject::connect(&notifier, &AlarmNotifier::criticalAlarm,
                     [&notifiedId](const AlarmEvent& ev) { notifiedId = ev.id; });

    eng.loadRules({makeRule(1, 50.0f, 40.0f, AlarmLevel::Critical)});
    eng.onDataUpdated(1, nowMs(), 66.0f);

    REQUIRE(notifier.criticalReceived() == 1);
    REQUIRE(notifier.notifyCount() == 1);
    REQUIRE(notifier.suppressedByStorm() == 0);
    REQUIRE(notifiedId == notifier.lastEvent().id);
    REQUIRE(notifier.lastEvent().level == AlarmLevel::Critical);
}

TEST_CASE("alarm notifier: warning and info are ignored", "[master][ui][alarm-notifier]") {
    AlarmEngine eng;
    AlarmNotifier notifier(&eng);

    eng.loadRules({makeRule(1, 50.0f, 40.0f, AlarmLevel::Warning),
                   makeRule(2, 50.0f, 40.0f, AlarmLevel::Info)});
    eng.onDataUpdated(1, nowMs(), 66.0f);
    eng.onDataUpdated(2, nowMs(), 66.0f);

    REQUIRE(notifier.criticalReceived() == 0);
    REQUIRE(notifier.notifyCount() == 0);
}

TEST_CASE("alarm notifier: debounce collapses burst within 1s window", "[master][ui][alarm-notifier]") {
    AlarmEngine eng;
    AlarmNotifier notifier(&eng);

    int notifiedN = 0;
    QObject::connect(&notifier, &AlarmNotifier::criticalAlarm,
                     [&notifiedN](const AlarmEvent&) { ++notifiedN; });

    eng.loadRules({makeRule(1, 50.0f, 40.0f, AlarmLevel::Critical),
                   makeRule(2, 50.0f, 40.0f, AlarmLevel::Critical),
                   makeRule(3, 50.0f, 40.0f, AlarmLevel::Critical),
                   makeRule(4, 50.0f, 40.0f, AlarmLevel::Critical)});
    eng.onDataUpdated(1, nowMs(), 66.0f);   // 首条 → 通知
    eng.onDataUpdated(2, nowMs() + 100, 66.0f);   // 窗口内 → 抑制（仅累计）
    eng.onDataUpdated(3, nowMs() + 200, 66.0f);   // 窗口内 → 抑制

    REQUIRE(notifier.criticalReceived() == 3);
    REQUIRE(notifier.notifyCount() == 1);
    REQUIRE(notifiedN == 1);

    sleepMs(1100);                          // 窗口过后 → 恢复（用新点 4：点 1 仍 Active 不重复触发）
    eng.onDataUpdated(4, nowMs() + 1500, 70.0f);
    REQUIRE(notifier.notifyCount() == 2);
    REQUIRE(notifiedN == 2);
}

TEST_CASE("alarm notifier: storm mode suppresses popup", "[master][ui][alarm-notifier]") {
    AlarmEngine eng;
    AlarmNotifier notifier(&eng);

    // threshold=0：首次越界即进入风暴（ring 计数 1 > 0）
    eng.setStormConfig({/*windowMs=*/1000, /*threshold=*/0, /*flushIntervalMs=*/50});
    eng.loadRules({makeRule(1, 50.0f, 40.0f, AlarmLevel::Critical)});
    eng.onDataUpdated(1, nowMs(), 66.0f);

    REQUIRE(eng.isInStormMode());
    REQUIRE(notifier.criticalReceived() == 1);
    REQUIRE(notifier.notifyCount() == 0);          // 风暴抑制，不弹窗
    REQUIRE(notifier.suppressedByStorm() == 1);
}
