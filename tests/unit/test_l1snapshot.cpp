// tests/unit/test_l1snapshot.cpp
// L3 数据中枢 ── L1SnapshotStore Tier 2 单测（ENS-LLD-200 §3.4/3.4.1 + Phase 3 4.1.2）。
//
// 覆盖：
//   ① initFromPolicy 空 policy 返 false
//   ② 稠密场景(连续 ID):isDense()=true,registeredCount==N,hasPoint
//   ③ 稀疏场景(大跨度 ID):isDense()=false,QHash 回退
//   ④ write + readRecent 路径:写值后读出一致
//   ⑤ 多测点独立 Buffer:每个 pointId 自己的数据
//   ⑥ 边界:未注册测点 readRecent 返 0,write 静默丢弃不崩
//   ⑦ capacityForPolicy 计算:典型 (sampleRate, retention) → 2 幂容量
//   ⑧ extractRange 端到端:write → extractRange 命中

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdint>
#include <vector>

#include "datahub/L1SnapshotStore.h"
#include "datahub/Sample.h"

using Catch::Approx;
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

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ① 空 policy
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("l1snapshot: initFromPolicy with empty policy returns false",
          "[master][datahub][l1snapshot][init][neg]") {
    L1SnapshotStore s;
    REQUIRE_FALSE(s.initFromPolicy({}));
    REQUIRE(s.registeredCount() == 0u);
    REQUIRE_FALSE(s.isDense());
}

// ─────────────────────────────────────────────────────────────────────────────
// ② 稠密场景(连续 ID)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("l1snapshot: dense path with contiguous IDs uses array indexing",
          "[master][datahub][l1snapshot][dense]") {
    L1SnapshotStore s;
    QVector<RingBufferPolicyEntry> policy;
    for (uint32_t i = 1; i <= 10; ++i) {
        policy.append({i, 100, 3'600'000, 1});
    }
    REQUIRE(s.initFromPolicy(policy));
    REQUIRE(s.isDense());
    REQUIRE(s.registeredCount() == 10u);
    for (uint32_t i = 1; i <= 10; ++i) {
        REQUIRE(s.hasPoint(i));
    }
    REQUIRE_FALSE(s.hasPoint(11));
    REQUIRE_FALSE(s.hasPoint(0));
}

// ─────────────────────────────────────────────────────────────────────────────
// ③ 稀疏场景(大跨度 ID)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("l1snapshot: sparse path with large range falls back to QHash",
          "[master][datahub][l1snapshot][sparse]") {
    L1SnapshotStore s;
    QVector<RingBufferPolicyEntry> policy;
    policy.append({1,    100, 3'600'000, 0});
    policy.append({1000, 100, 3'600'000, 0});
    policy.append({50000,100, 3'600'000, 0});        // 跨度 50000,只有 3 个点 → 稀疏
    REQUIRE(s.initFromPolicy(policy));
    REQUIRE_FALSE(s.isDense());
    REQUIRE(s.registeredCount() == 3u);
    REQUIRE(s.hasPoint(1));
    REQUIRE(s.hasPoint(1000));
    REQUIRE(s.hasPoint(50000));
    REQUIRE_FALSE(s.hasPoint(25000));                  // 跨度内未注册的 ID
}

// ─────────────────────────────────────────────────────────────────────────────
// ④ write + readRecent 路径
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("l1snapshot: write then readRecent round-trips samples",
          "[master][datahub][l1snapshot][rw]") {
    L1SnapshotStore s;
    QVector<RingBufferPolicyEntry> policy;
    policy.append({1, 100, 3'600'000, 1});
    REQUIRE(s.initFromPolicy(policy));

    for (int i = 1; i <= 50; ++i) {
        s.write(1, mkSample(static_cast<uint64_t>(i), 1, static_cast<float>(i) * 0.5f));
    }
    Sample out[50];
    const size_t rc = s.readRecent(1, /*cid=*/0, out, 50);
    REQUIRE(rc == 50u);
    REQUIRE(out[0].timestamp == 1u);
    REQUIRE(out[49].timestamp == 50u);
    REQUIRE(out[49].value == Approx(25.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑤ 多测点独立 Buffer
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("l1snapshot: per-point buffers are isolated",
          "[master][datahub][l1snapshot][multi]") {
    L1SnapshotStore s;
    QVector<RingBufferPolicyEntry> policy;
    policy.append({1, 100, 3'600'000, 1});
    policy.append({2, 100, 3'600'000, 1});
    policy.append({3, 100, 3'600'000, 1});
    REQUIRE(s.initFromPolicy(policy));

    s.write(1, mkSample(100, 1, 1.0f));
    s.write(2, mkSample(200, 2, 2.0f));
    s.write(3, mkSample(300, 3, 3.0f));

    Sample out[1];
    REQUIRE(s.readRecent(1, 0, out, 1) == 1u);
    REQUIRE(out[0].timestamp == 100u);                 // 测点 1 独立
    REQUIRE(s.readRecent(2, 0, out, 1) == 1u);
    REQUIRE(out[0].timestamp == 200u);                 // 测点 2 独立
    REQUIRE(s.readRecent(3, 0, out, 1) == 1u);
    REQUIRE(out[0].timestamp == 300u);                 // 测点 3 独立
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑥ 未注册测点
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("l1snapshot: unknown pointId: write silently drops, readRecent returns 0",
          "[master][datahub][l1snapshot][unknown]") {
    L1SnapshotStore s;
    QVector<RingBufferPolicyEntry> policy;
    policy.append({1, 100, 3'600'000, 1});
    REQUIRE(s.initFromPolicy(policy));

    // write 未注册测点:不崩,静默丢弃
    REQUIRE_NOTHROW(s.write(999, mkSample(1, 999, 0.0f)));
    Sample out[1];
    REQUIRE(s.readRecent(999, 0, out, 1) == 0u);       // 未注册无数据
    REQUIRE(s.readRecent(1, 0, out, 1) == 0u);         // 注册点也仍空
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑦ capacityForPolicy 容量预算(LLD §3.5.2 NFR-PERF-05 表格)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("l1snapshot: capacityForPolicy maps (rate, retention) to clamped power-of-2",
          "[master][datahub][l1snapshot][capacity]") {
    // BMS 极速 100ms/1h → 36000 slots → nextPow2=65536,在 [256,65536] 内
    REQUIRE(L1SnapshotStore::capacityForPolicy({1, 100, 3'600'000, 0}) == 65536u);
    // 普通遥测 1s/30min → 1800 slots → 2048
    REQUIRE(L1SnapshotStore::capacityForPolicy({1, 1000, 1'800'000, 1}) == 2048u);
    // 低频 5s/15min → 180 slots → 256(下钳)
    REQUIRE(L1SnapshotStore::capacityForPolicy({1, 5000, 900'000, 2}) == 256u);
    // 超小 sampleRate=0 兜底 256
    REQUIRE(L1SnapshotStore::capacityForPolicy({1, 0, 1000, 2}) == 256u);
    // 超大 50ms/24h → 1,728,000 → 上钳 65536
    REQUIRE(L1SnapshotStore::capacityForPolicy({1, 50, 86'400'000, 0}) == 65536u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑧ extractRange 端到端
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("l1snapshot: extractRange hits time window across written samples",
          "[master][datahub][l1snapshot][range]") {
    L1SnapshotStore s;
    QVector<RingBufferPolicyEntry> policy;
    policy.append({1, 100, 3'600'000, 1});
    REQUIRE(s.initFromPolicy(policy));

    for (uint64_t t = 10; t <= 100; t += 10) {
        s.write(1, mkSample(t, 1, 0.0f));
    }
    Sample out[10];
    const size_t n = s.extractRange(1, 30, 60, out, 10);   // [30, 60] → 30,40,50,60
    REQUIRE(n == 4u);
    REQUIRE(out[0].timestamp == 30u);
    REQUIRE(out[3].timestamp == 60u);
}
