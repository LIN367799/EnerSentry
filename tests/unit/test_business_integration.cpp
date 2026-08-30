// tests/unit/test_business_integration.cpp
// L4 业务层 ── 联调 Tier 3 测试（ENS-LLD-400 §3.5 + DevGuide §4.3.4 + 截图要求）。

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDateTime>
#include <QTemporaryDir>

#include <chrono>
#include <thread>

#include "datahub/BlackBoxManager.h"
#include "datahub/CriticalSwapFile.h"
#include "datahub/L1SnapshotStore.h"
#include "datahub/Sample.h"

#include "business/AlarmEngine.h"

using ens::business::AlarmEngine;
using ens::business::AlarmLevel;
using ens::business::AlarmRule;
using ens::datahub::BlackBoxManager;
using ens::datahub::CriticalSwapFile;
using ens::datahub::L1SnapshotStore;
using ens::datahub::RingBufferPolicyEntry;
using ens::datahub::Sample;

namespace {
Sample mkSample(uint64_t ts, uint32_t pid, float v) {
    Sample s;
    s.timestamp = ts;
    s.pointId   = pid;
    s.value     = v;
    return s;
}
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void processEvents(int ms = 50) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, ms);
}
}  // namespace

// Critical 告警 → AlarmEngine::blackBoxRequested → BlackBoxManager::triggerBlackBox
TEST_CASE("business_integration: Critical alarm triggers mmap snapshot via BlackBox",
          "[master][business][integration][critical-blackbox][tier3]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());

    // L3 数据中枢
    L1SnapshotStore l1;
    QVector<RingBufferPolicyEntry> policy;
    policy.append({1, 100, 3'600'000, 0});
    REQUIRE(l1.initFromPolicy(policy));

    // L1 填充真实 epoch 时间窗(与 AlarmEngine 的 triggerTime=system_clock 同一时间基)
    const uint64_t alarmTs = static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
    for (uint64_t t = alarmTs - 30000; t <= alarmTs + 30000; t += 100) {
        l1.write(1, mkSample(t, 1, 1.0f));
    }

    BlackBoxManager bbMgr(&l1);
    REQUIRE(bbMgr.enableCriticalSwap(tmp.path() + "/swap.bin"));

    // L4 业务
    AlarmEngine alarm;
    AlarmRule r;
    r.pointId      = 1;
    r.level        = AlarmLevel::Critical;
    r.onThreshold  = 0.5f;
    r.offThreshold = 0.1f;
    r.onDelayMs    = 50;
    r.offDelayMs   = 50;
    r.enabled      = true;
    alarm.loadRules({r});

    // 联调：Critical 告警 → triggerBlackBox（依赖倒置：业务层 emit，外部连 datahub）
    auto onBlackBox = [&](uint32_t pid, uint64_t alarmTimeEpoch) {
        bbMgr.triggerBlackBox(pid, alarmTimeEpoch, ens::datahub::AlarmLevel::Critical);
    };
    QObject::connect(&alarm, &AlarmEngine::blackBoxRequested, onBlackBox);

    int triggeredN = 0;
    QObject::connect(&alarm, &AlarmEngine::alarmTriggered,
                     [&triggeredN](const ens::business::AlarmEvent&) { ++triggeredN; });

    alarm.onDataUpdated(1, alarmTs, 1.0f);
    sleepMs(80);
    alarm.onDataUpdated(1, alarmTs + 100, 1.0f);
    processEvents();

    REQUIRE(triggeredN == 1);
    REQUIRE(bbMgr.criticalTriggerCount() >= 1u);

    // 强制 flushSync + close mmap 句柄(让 reread 可重新打开)
    bbMgr.forceSync();

    // 重开 mmap 文件解析
    CriticalSwapFile reread;
    REQUIRE(reread.open(tmp.path() + "/swap.bin"));
    REQUIRE(reread.snapshotCount() >= 1u);
    const auto pending = reread.parsePendingSnapshots();
    REQUIRE(pending.size() == 1u);
    REQUIRE(pending[0].pointId == 1u);
    REQUIRE(pending[0].level   == ens::datahub::AlarmLevel::Critical);
}