// tests/unit/test_down_sampler.cpp
// L3 数据中枢 ── DownSampler Tier 2 单测（ENS-LLD-200 §5.1 + Phase 3 4.1.4）。
//
// 覆盖：
//   ① alignToWindow:对齐到粒度边界
//   ② windowMs:粒度→毫秒
//   ③ feed 单点聚合:Min/Max/Avg/Sum/Count/First/Last
//   ④ 多测点独立桶
//   ⑤ rollUp 窗口闭合判定(nowMs 控制)
//   ⑥ bucketCount 诊断
//   ⑦ 边界:ts==0 静默忽略;未闭合桶不 rollUp

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "datahub/DownSampler.h"
#include "datahub/Sample.h"

using Catch::Approx;
using ens::datahub::Bucket;
using ens::datahub::DownSampler;
using ens::datahub::DownSampledSample;
using ens::datahub::HistoryGranularity;
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
// ① alignToWindow / windowMs
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("down_sampler: alignToWindow floors ts to granularity boundary",
          "[master][datahub][downsampler][align]") {
    using ens::datahub::DownSampler;
    REQUIRE(DownSampler::alignToWindow(1234, HistoryGranularity::Gran1s) == 1000u);
    REQUIRE(DownSampler::alignToWindow(5234, HistoryGranularity::Gran5s) == 5000u);
    REQUIRE(DownSampler::alignToWindow(1234, HistoryGranularity::Gran1m) == 0u);
    REQUIRE(DownSampler::alignToWindow(1234, HistoryGranularity::Gran100ms) == 1200u);
    // 边界:整边界对齐到自身
    REQUIRE(DownSampler::alignToWindow(5000, HistoryGranularity::Gran5s) == 5000u);
    REQUIRE(DownSampler::alignToWindow(0, HistoryGranularity::Gran1s) == 0u);
}

TEST_CASE("down_sampler: windowMs maps granularity to milliseconds",
          "[master][datahub][downsampler][win]") {
    using ens::datahub::DownSampler;
    REQUIRE(DownSampler::windowMs(HistoryGranularity::Gran100ms) == 100u);
    REQUIRE(DownSampler::windowMs(HistoryGranularity::Gran1s)    == 1000u);
    REQUIRE(DownSampler::windowMs(HistoryGranularity::Gran5s)    == 5000u);
    REQUIRE(DownSampler::windowMs(HistoryGranularity::Gran1m)    == 60000u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ② feed 单点聚合
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("down_sampler: feed aggregates Min/Max/Avg/Sum/Count/First/Last",
          "[master][datahub][downsampler][feed]") {
    DownSampler ds;
    const uint32_t pid = 1;
    // 1s 窗口(ts 1000..1999)
    ds.feed(pid, mkSample(1000, pid, 10.0f), HistoryGranularity::Gran1s);
    ds.feed(pid, mkSample(1100, pid, 30.0f), HistoryGranularity::Gran1s);
    ds.feed(pid, mkSample(1200, pid, 20.0f), HistoryGranularity::Gran1s);

    REQUIRE(ds.bucketCount(pid, HistoryGranularity::Gran1s) == 1u);
    const auto closed = ds.rollUp(pid, HistoryGranularity::Gran1s, /*nowMs=*/2000);
    REQUIRE(closed.size() == 1u);
    const auto& s = closed[0];
    REQUIRE(s.pointId == pid);
    REQUIRE(s.timestamp == 1000u);
    REQUIRE(s.maxValue == Approx(30.0f));
    REQUIRE(s.minValue == Approx(10.0f));
    REQUIRE(s.avgValue == Approx(20.0f));                // (10+30+20)/3 = 20
    REQUIRE(s.sampleCount == 3u);
}

TEST_CASE("down_sampler: feed ignores ts==0 (boundary)",
          "[master][datahub][downsampler][neg]") {
    DownSampler ds;
    ds.feed(1, mkSample(0, 1, 99.0f), HistoryGranularity::Gran1s);
    REQUIRE(ds.bucketCount(1, HistoryGranularity::Gran1s) == 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ③ 多测点独立桶
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("down_sampler: per-point buckets are isolated",
          "[master][datahub][downsampler][multi]") {
    DownSampler ds;
    // 同窗口不同测点
    ds.feed(1, mkSample(1000, 1, 1.0f), HistoryGranularity::Gran1s);
    ds.feed(2, mkSample(1000, 2, 2.0f), HistoryGranularity::Gran1s);
    ds.feed(1, mkSample(1100, 1, 3.0f), HistoryGranularity::Gran1s);

    REQUIRE(ds.bucketCount(1, HistoryGranularity::Gran1s) == 1u);
    REQUIRE(ds.bucketCount(2, HistoryGranularity::Gran1s) == 1u);
    auto p1 = ds.rollUp(1, HistoryGranularity::Gran1s, /*nowMs=*/2000);
    auto p2 = ds.rollUp(2, HistoryGranularity::Gran1s, /*nowMs=*/2000);
    REQUIRE(p1.size() == 1u);
    REQUIRE(p2.size() == 1u);
    REQUIRE(p1[0].maxValue == Approx(3.0f));              // 测点 1
    REQUIRE(p2[0].maxValue == Approx(2.0f));              // 测点 2
}

// ─────────────────────────────────────────────────────────────────────────────
// ④ rollUp 窗口闭合判定
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("down_sampler: rollUp closes only windows before nowWin",
          "[master][datahub][downsampler][rollup]") {
    DownSampler ds;
    // 三个连续 1s 窗口
    ds.feed(1, mkSample(1000, 1, 1.0f), HistoryGranularity::Gran1s);
    ds.feed(1, mkSample(2000, 1, 2.0f), HistoryGranularity::Gran1s);
    ds.feed(1, mkSample(3000, 1, 3.0f), HistoryGranularity::Gran1s);

    // nowMs=2500 → 对齐 nowWin=2000,只闭合 win=1000(1.0)
    auto part = ds.rollUp(1, HistoryGranularity::Gran1s, /*nowMs=*/2500);
    REQUIRE(part.size() == 1u);
    REQUIRE(part[0].timestamp == 1000u);
    REQUIRE(part[0].maxValue == Approx(1.0f));
    REQUIRE(ds.bucketCount(1, HistoryGranularity::Gran1s) == 2u);  // win=2000,3000 仍存活

    // nowMs=4000 → 闭合剩余两个
    auto rest = ds.rollUp(1, HistoryGranularity::Gran1s, /*nowMs=*/4000);
    REQUIRE(rest.size() == 2u);
    REQUIRE(ds.bucketCount(1, HistoryGranularity::Gran1s) == 0u);
}

TEST_CASE("down_sampler: rollUp with default nowMs=0 uses system time (no closed windows)",
          "[master][datahub][downsampler][rollup][now]") {
    DownSampler ds;
    ds.feed(1, mkSample(/*ts=*/1000, 1, 1.0f), HistoryGranularity::Gran1s);
    // 不传 nowMs → 用系统时间(1970 之后所有窗口都已闭合)
    auto all = ds.rollUp(1, HistoryGranularity::Gran1s);
    REQUIRE(all.size() == 1u);
    REQUIRE(all[0].timestamp == 1000u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑤ 边界:无桶/空桶
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("down_sampler: rollUp on unknown point returns empty",
          "[master][datahub][downsampler][empty]") {
    DownSampler ds;
    auto empty = ds.rollUp(999, HistoryGranularity::Gran1s, /*nowMs=*/10000);
    REQUIRE(empty.empty());
}

TEST_CASE("down_sampler: clear empties all buckets",
          "[master][datahub][downsampler][clear]") {
    DownSampler ds;
    ds.feed(1, mkSample(1000, 1, 1.0f), HistoryGranularity::Gran1s);
    ds.feed(2, mkSample(2000, 2, 2.0f), HistoryGranularity::Gran1s);
    REQUIRE(ds.bucketCount(1, HistoryGranularity::Gran1s) == 1u);
    ds.clear();
    REQUIRE(ds.bucketCount(1, HistoryGranularity::Gran1s) == 0u);
    REQUIRE(ds.bucketCount(2, HistoryGranularity::Gran1s) == 0u);
}
