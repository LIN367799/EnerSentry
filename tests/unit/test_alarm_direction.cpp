// tests/unit/test_alarm_direction.cpp —— AlarmDirection 低告警单测（切片 29）。
// 覆盖：Low 方向跌破 onThreshold 触发 / 迟滞带维持 / 回升 offThreshold 以上恢复 /
//       高告警（默认 High）行为不变 / JSON 加载 direction 解析。
// ⚠ TEST_CASE 第一参数严格 ASCII（项目测试铁律）。
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <filesystem>
#include <thread>

#include "business/AlarmEngine.h"
#include "business/AlarmRuleLoader.h"
#include "protocol/PointTable.h"

using ens::business::AlarmEngine;
using ens::business::AlarmLevel;
using ens::business::AlarmRule;
using ens::business::AlarmDirection;

namespace {
AlarmRule makeLowRule(uint32_t pid, float on, float off,
                      uint32_t onDelayMs = 100, uint32_t offDelayMs = 100) {
    AlarmRule r;
    r.pointId      = pid;
    r.level        = AlarmLevel::Warning;
    r.direction    = AlarmDirection::Low;
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

TEST_CASE("alarm_direction: low rule raises below onThreshold and recovers above off",
          "[master][business][alarm][direction][tier2]") {
    AlarmEngine eng;
    // SOC 例：低于 20% 告警，回升到 25% 以上恢复（迟滞带 [20,25]）
    eng.loadRules({makeLowRule(2, 20.0f, 25.0f, 100, 100)});

    int triggeredN = 0;
    int recoveredN = 0;
    QObject::connect(&eng, &AlarmEngine::alarmTriggered,
                     [&triggeredN](const ens::business::AlarmEvent&) { ++triggeredN; });
    QObject::connect(&eng, &AlarmEngine::alarmRecovered,
                     [&recoveredN](uint64_t) { ++recoveredN; });

    // 18%：跌破 20% → on-delay（未达 100ms 不触发）
    eng.onDataUpdated(2, 1000, 18.0f);
    processEvents();
    REQUIRE(triggeredN == 0);

    sleepMs(150);
    eng.onDataUpdated(2, 1100, 18.0f);
    processEvents();
    REQUIRE(triggeredN == 1);
    REQUIRE(eng.isPointInAlarm(2));

    // 22%：迟滞带 [20,25] 内 → 维持不恢复
    eng.onDataUpdated(2, 1200, 22.0f);
    processEvents();
    REQUIRE(eng.activeAlarmCount() == 1);

    // 28%：回升超 25% → off-delay → 恢复
    eng.onDataUpdated(2, 1300, 28.0f);
    sleepMs(150);
    eng.onDataUpdated(2, 1400, 28.0f);
    processEvents();
    REQUIRE(recoveredN == 1);
    REQUIRE_FALSE(eng.isPointInAlarm(2));
}

TEST_CASE("alarm_direction: high direction behavior unchanged (regression)",
          "[master][business][alarm][direction][tier2]") {
    AlarmEngine eng;
    AlarmRule r;
    r.pointId = 1; r.level = AlarmLevel::Warning;
    r.direction = AlarmDirection::High;   // 默认方向
    r.onThreshold = 10.0f; r.offThreshold = 5.0f;
    r.onDelayMs = 100; r.offDelayMs = 100;
    eng.loadRules({r});

    int triggeredN = 0;
    QObject::connect(&eng, &AlarmEngine::alarmTriggered,
                     [&triggeredN](const ens::business::AlarmEvent&) { ++triggeredN; });

    eng.onDataUpdated(1, 1000, 12.0f);   // > 10 → 越上阈
    sleepMs(150);
    eng.onDataUpdated(1, 1100, 12.0f);
    processEvents();
    REQUIRE(triggeredN == 1);
}

TEST_CASE("alarm_direction: JSON loader parses direction with default high",
          "[master][business][alarm][direction][loader][tier2]") {
    auto pt = ens::protocol::PointTable::loadFromJsonFile(
        std::filesystem::path("../test_data/sim_pointtable_sample.json"));
    REQUIRE(pt != nullptr);

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QFile f(dir.filePath(QStringLiteral("rules.json")));
    REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(R"({
        "rules": [
            {"pointName":"Rack-01_MaxTemp","level":"Warning","onThreshold":60,"offThreshold":40},
            {"pointName":"Rack-01_SOC","level":"Warning","onThreshold":20,"offThreshold":25,"direction":"low"}
        ]
    })");
    f.close();

    std::vector<ens::business::AlarmRule> rules;
    std::string err;
    const int n = ens::business::AlarmRuleLoader::loadFromFile(
        dir.filePath(QStringLiteral("rules.json")).toStdString(), *pt, rules, &err);
    INFO("n=" << n << " err=" << err << " ptPoints=" << pt->allPoints().size());
    REQUIRE(n == 2);
    REQUIRE(rules[0].direction == AlarmDirection::High);   // 缺省 → High（兼容）
    REQUIRE(rules[1].direction == AlarmDirection::Low);
}
