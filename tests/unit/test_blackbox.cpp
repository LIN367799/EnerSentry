// tests/unit/test_blackbox.cpp
// L3 数据中枢 ── BlackBoxManager Tier 3 单测（ENS-LLD-200 §3.6 + Phase 3 4.1.6）。
//
// 覆盖：
//   ① enableCriticalSwap + triggerBlackBox(Critical) → mmap 快照可解析
//   ② 样本 ±30s 窗口提取正确(数量 + 内容)
//   ③ 重启恢复:close 后重开 mmap 文件 → 快照仍可读(持久性)
//   ④ 多快照环形覆盖(> kSlotCount 不越界)
//   ⑤ mmap 未启用时降级(trigger 仍计数,不崩)

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#include <QTemporaryDir>

#include "datahub/BlackBoxManager.h"
#include "datahub/CriticalSwapFile.h"
#include "datahub/L1SnapshotStore.h"
#include "datahub/Sample.h"

using ens::datahub::AlarmLevel;
using ens::datahub::BlackBoxManager;
using ens::datahub::BlackBoxSnapshot;
using ens::datahub::CriticalSwapFile;
using ens::datahub::CriticalSwapRecovery;
using ens::datahub::L1SnapshotStore;
using ens::datahub::RingBufferPolicyEntry;
using ens::datahub::Sample;

namespace {

class TempDir {
public:
    TempDir() : m_tmp(std::make_unique<QTemporaryDir>()) {
        if (!m_tmp->isValid()) throw std::runtime_error("QTemporaryDir failed");
    }
    QString path() const { return m_tmp->path(); }
private:
    std::unique_ptr<QTemporaryDir> m_tmp;
};

Sample mkSample(uint64_t ts, uint32_t pid, float v) {
    Sample s;
    s.timestamp = ts;
    s.pointId   = pid;
    s.value     = v;
    return s;
}

/// 就地初始化带 ±30s 数据的 L1SnapshotStore(L1SnapshotStore 禁 move,用引用传参)
void makeStore(L1SnapshotStore& store) {
    QVector<RingBufferPolicyEntry> policy;
    policy.append({1, 100, 3'600'000, 0});
    store.initFromPolicy(policy);
    const uint64_t alarmTs = 1000000000000ULL;             // 告警时间
    for (uint64_t t = alarmTs - 30000; t <= alarmTs + 30000; t += 100) {
        store.write(1, mkSample(t, 1, 1.0f));
    }
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ① enableCriticalSwap + triggerBlackBox(Critical) → mmap 快照可解析
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("blackbox: critical trigger persists snapshot to mmap swap",
          "[master][datahub][blackbox][tier3]") {
    TempDir tmp;
    L1SnapshotStore store;
    makeStore(store);
    BlackBoxManager mgr(&store);
    REQUIRE(mgr.enableCriticalSwap(tmp.path() + "/swap.bin"));

    const uint64_t alarmTs = 1000000000000ULL;
    const auto snap = mgr.triggerBlackBox(/*pointId=*/1, alarmTs, AlarmLevel::Critical);

    REQUIRE(snap.pointId == 1u);
    REQUIRE(snap.level == AlarmLevel::Critical);
    REQUIRE_FALSE(snap.samples.empty());
    REQUIRE(mgr.criticalTriggerCount() == 1u);
    REQUIRE(mgr.criticalSnapshotCount() == 1u);

    // 重开文件解析
    CriticalSwapFile reread;
    REQUIRE(reread.open(tmp.path() + "/swap.bin"));
    const auto pending = reread.parsePendingSnapshots();
    REQUIRE(pending.size() == 1u);
    REQUIRE(pending[0].pointId == 1u);
    REQUIRE(pending[0].samples.size() == snap.samples.size());
    REQUIRE(pending[0].samples[0].timestamp == alarmTs - 30000);
}

// ─────────────────────────────────────────────────────────────────────────────
// ② 样本窗口 ±30s
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("blackbox: pre-copy window spans alarmTs +/- 30s",
          "[master][datahub][blackbox][window]") {
    TempDir tmp;
    L1SnapshotStore store;
    makeStore(store);
    BlackBoxManager mgr(&store);
    REQUIRE(mgr.enableCriticalSwap(tmp.path() + "/swap2.bin"));

    const uint64_t alarmTs = 1000000000000ULL;
    const auto snap = mgr.triggerBlackBox(1, alarmTs, AlarmLevel::Warning);   // 非 Critical 也预拷贝

    // ±30s @100ms → 601 个样本(含告警时刻)
    REQUIRE(snap.samples.size() == 601u);
    REQUIRE(snap.samples.front().timestamp == alarmTs - 30000);
    REQUIRE(snap.samples.back().timestamp  == alarmTs + 30000);
}

// ─────────────────────────────────────────────────────────────────────────────
// ③ 重启恢复(持久性)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("blackbox: CriticalSwapRecovery reopens persisted swap after close",
          "[master][datahub][blackbox][recovery][tier3]") {
    TempDir tmp;
    L1SnapshotStore store;
    makeStore(store);
    const QString swapPath = tmp.path() + "/swap3.bin";

    {
        BlackBoxManager mgr(&store);
        REQUIRE(mgr.enableCriticalSwap(swapPath));
        mgr.triggerBlackBox(1, 1000000000000ULL, AlarmLevel::Critical);
        mgr.triggerBlackBox(1, 1000000001000ULL, AlarmLevel::Critical);
    }   // mgr 析构 → swap close

    // 新会话恢复
    const auto result = CriticalSwapRecovery::start(swapPath);
    REQUIRE(result.recovered);
    REQUIRE(result.pendingSnapshots >= 1);

    CriticalSwapFile reread;
    REQUIRE(reread.open(swapPath));
    REQUIRE(reread.snapshotCount() == 2u);
    const auto pending = reread.parsePendingSnapshots();
    REQUIRE(pending.size() == 2u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ④ 多快照环形覆盖
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("blackbox: ring wrap beyond slot count does not corrupt",
          "[master][datahub][blackbox][ring]") {
    TempDir tmp;
    L1SnapshotStore store;
    makeStore(store);
    BlackBoxManager mgr(&store);
    REQUIRE(mgr.enableCriticalSwap(tmp.path() + "/swap4.bin"));

    // 写入超过槽位数(64)的快照 → 环形覆盖,不越界
    for (int i = 0; i < 80; ++i) {
        mgr.triggerBlackBox(1, 1000000000000ULL + i, AlarmLevel::Critical);
    }
    REQUIRE(mgr.criticalSnapshotCount() == 80u);   // 计数不封顶
    // 重开解析:最多 kMaxPending(16) 个
    CriticalSwapFile reread;
    REQUIRE(reread.open(tmp.path() + "/swap4.bin"));
    const auto pending = reread.parsePendingSnapshots();
    REQUIRE(pending.size() <= 16u);
    REQUIRE_FALSE(pending.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑤ mmap 未启用降级
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("blackbox: critical trigger without swap still counts, no crash",
          "[master][datahub][blackbox][degraded]") {
    L1SnapshotStore store;
    makeStore(store);
    BlackBoxManager mgr(&store);      // 未 enableCriticalSwap

    const auto snap = mgr.triggerBlackBox(1, 1000000000000ULL, AlarmLevel::Critical);
    REQUIRE(snap.level == AlarmLevel::Critical);
    REQUIRE(mgr.criticalTriggerCount() == 1u);
    REQUIRE_FALSE(mgr.criticalSwapEnabled());
    REQUIRE(mgr.criticalSnapshotCount() == 0u);   // mmap 未启用,无持久化
}
