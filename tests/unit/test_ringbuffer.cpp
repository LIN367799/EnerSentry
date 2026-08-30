// tests/unit/test_ringbuffer.cpp
// L3 数据中枢 ── RingBuffer<T> Tier 2 单测（ENS-LLD-200 §3.2/3.5.1 + Phase 3 4.1.1）。
//
// 覆盖：
//   ① 容量合法性:2 幂 OK;非 2 幂/越界抛 std::invalid_argument
//   ② push 1000 顺序一致(单消费者 readRecent)
//   ③ 2×容量溢出:readRecent 自动跳到最老可读(返 capacity 个,无越界)
//   ④ 多消费者隔离(cid 0/1 各自读、各自游标独立)
//   ⑤ pushBatch:写入 N 个后 latestPublished 跳到末尾、读出顺序与写入顺序一致
//   ⑥ extractRange:时间窗边界(startTs > endTs 返 0、命中窗口、跨右边界截断)
//   ⑦ evictSlowConsumer:正常返 true;过慢返 false 且游标跳到最老可读
//   ⑧ 边界:out==nullptr / count==0 / 越界 consumerId 返 0

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "datahub/RingBuffer.h"
#include "datahub/Sample.h"

using Catch::Approx;
using ens::datahub::RingBuffer;
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
// ① 容量合法性
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ringbuffer: power-of-2 capacity accepted; non-power-of-2 throws",
          "[master][datahub][ringbuffer][cap]") {
    REQUIRE_NOTHROW(RingBuffer<Sample>(64));
    REQUIRE_NOTHROW(RingBuffer<Sample>(256));
    REQUIRE_NOTHROW(RingBuffer<Sample>(65536));

    REQUIRE_THROWS_AS(RingBuffer<Sample>(0), std::invalid_argument);
    REQUIRE_THROWS_AS(RingBuffer<Sample>(1), std::invalid_argument);
    REQUIRE_THROWS_AS(RingBuffer<Sample>(100), std::invalid_argument);   // 非 2 幂
    REQUIRE_THROWS_AS(RingBuffer<Sample>(65537), std::invalid_argument); // 越界
}

// ─────────────────────────────────────────────────────────────────────────────
// ② push 1000 顺序一致
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ringbuffer: push 1000 sequential samples, single consumer reads in order",
          "[master][datahub][ringbuffer][seq]") {
    RingBuffer<Sample> rb(1024, "seq");
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        rb.push(mkSample(static_cast<uint64_t>(i + 1), /*pid=*/1, static_cast<float>(i)));
    }
    // counter 语义:推 N 次后 published = N(已发布数据量)
    REQUIRE(rb.latestPublished() == static_cast<size_t>(N));

    std::vector<Sample> out(N);
    const size_t read = rb.readRecent(/*cid=*/0, out.data(), N);
    REQUIRE(read == static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        REQUIRE(out[i].timestamp == static_cast<uint64_t>(i + 1));
        REQUIRE(out[i].value == Approx(static_cast<float>(i)));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ③ 2×容量溢出:自动跳到最老可读
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ringbuffer: overflow beyond capacity drops oldest, returns capacity samples",
          "[master][datahub][ringbuffer][overflow]") {
    constexpr size_t CAP = 64;
    RingBuffer<Sample> rb(CAP, "overflow");

    // 推 CAP * 2 个样本(ts 1..128)→published=128;读 64 个应是最新 64(ts 65..128)
    constexpr int N = 128;
    for (int i = 0; i < N; ++i) {
        rb.push(mkSample(static_cast<uint64_t>(i + 1), 1, 0.0f));
    }
    REQUIRE(rb.latestPublished() == static_cast<size_t>(N));

    // 读 CAP 个:应是最新的 64 个(ts 65..128)
    std::vector<Sample> out(CAP);
    const size_t read = rb.readRecent(0, out.data(), CAP);
    REQUIRE(read == CAP);
    REQUIRE(out.front().timestamp == 65u);
    REQUIRE(out.back().timestamp  == 128u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ④ 多消费者隔离
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ringbuffer: multi-consumer cursors are independent",
          "[master][datahub][ringbuffer][multi-consumer]") {
    RingBuffer<Sample> rb(256, "multi");
    for (int i = 0; i < 10; ++i) rb.push(mkSample(static_cast<uint64_t>(i + 1), 1, 0.0f));

    // 推 10 次后:published=9(0-based)
    // consumer 0 读 5 个 → cursor 推进到 4(0-based 末尾已读)
    Sample a[5]; REQUIRE(rb.readRecent(0, a, 5) == 5);
    // consumer 1 独立读:应从 ts 1 开始读 10 个
    Sample b[10]; REQUIRE(rb.readRecent(1, b, 10) == 10);
    REQUIRE(b[0].timestamp == 1u);
    REQUIRE(b[9].timestamp == 10u);
    // consumer 0 再读:应从 ts 6 开始(自己 cursor 之后)
    Sample c[10];
    const size_t rc = rb.readRecent(0, c, 10);
    REQUIRE(rc == 5);
    REQUIRE(c[0].timestamp == 6u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑤ pushBatch
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ringbuffer: pushBatch writes N samples, latestPublished advances to end",
          "[master][datahub][ringbuffer][batch]") {
    RingBuffer<Sample> rb(128, "batch");
    std::vector<Sample> src(20);
    for (int i = 0; i < 20; ++i) {
        src[i] = mkSample(static_cast<uint64_t>(i + 100), 1, 0.0f);
    }
    rb.pushBatch(src.data(), 20);
    // counter 语义:pushBatch 20 个后 published = 20
    REQUIRE(rb.latestPublished() == 20u);

    Sample out[20];
    const size_t rc = rb.readRecent(0, out, 20);
    REQUIRE(rc == 20u);
    REQUIRE(out[0].timestamp == 100u);
    REQUIRE(out[19].timestamp == 119u);
}

TEST_CASE("ringbuffer: pushBatch with nullptr/zero count is a no-op",
          "[master][datahub][ringbuffer][batch][neg]") {
    RingBuffer<Sample> rb(64, "batch-neg");
    rb.pushBatch(nullptr, 5);                          // 边界
    rb.pushBatch(nullptr, 0);                          // 边界
    REQUIRE(rb.latestPublished() == 0u);

    Sample out[1];
    REQUIRE(rb.readRecent(0, out, 1) == 0);            // 无数据
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑥ extractRange 时间窗
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ringbuffer: extractRange filters by [startTs, endTs] inclusive",
          "[master][datahub][ringbuffer][range]") {
    RingBuffer<Sample> rb(128, "range");
    for (uint64_t t = 10; t <= 100; t += 10) {
        rb.push(mkSample(t, 1, 0.0f));  // ts: 10, 20, ..., 100 (10 个)
    }
    Sample out[20];
    // 窗口 [30, 60] → ts 30,40,50,60 共 4 个
    const size_t n1 = rb.extractRange(30, 60, out, 20);
    REQUIRE(n1 == 4u);
    REQUIRE(out[0].timestamp == 30u);
    REQUIRE(out[3].timestamp == 60u);

    // startTs > endTs → 0
    REQUIRE(rb.extractRange(70, 60, out, 20) == 0u);
    // 完全左偏(startTs 远大于最大 ts)→ 0
    REQUIRE(rb.extractRange(200, 300, out, 20) == 0u);
    // maxCount 截断
    Sample small[2];
    const size_t n2 = rb.extractRange(20, 50, small, 2);
    REQUIRE(n2 == 2u);
    REQUIRE(small[0].timestamp == 20u);
    REQUIRE(small[1].timestamp == 30u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑦ evictSlowConsumer
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ringbuffer: evictSlowConsumer returns true normally; evicts on overrun",
          "[master][datahub][ringbuffer][evict]") {
    constexpr size_t CAP = 32;
    RingBuffer<Sample> rb(CAP, "evict");
    // 推 CAP+1=33 个 → published=33(已发布数据量), 游标落后 ≥ cap(32)
    for (uint64_t t = 1; t <= 33; ++t) rb.push(mkSample(t, 1, 0.0f));

    // 模拟落后消费者(从未读取, cursor=0);33-0=33 >= cap=32 → evict
    REQUIRE_FALSE(rb.evictSlowConsumer(0));
    // newCursor = published - cap = 33-32 = 1(跳到 pos 1;pos 0 = ts 1 已被 pos 32 覆盖)
    // readRecent: published=33, cursor=1, readable=min(33-1, 32)=32, idx 1..32 → pos 1..32 = ts 2..33
    Sample out[32];
    const size_t rc = rb.readRecent(0, out, 32);
    REQUIRE(rc == 32u);
    REQUIRE(out[0].timestamp == 2u);
    REQUIRE(out[31].timestamp == 33u);
}

TEST_CASE("ringbuffer: evictSlowConsumer returns true when cursor is current",
          "[master][datahub][ringbuffer][evict][ok]") {
    RingBuffer<Sample> rb(32, "evict-ok");
    rb.push(mkSample(1, 1, 0.0f));
    // 推 1 个,cursor=0(初始),落后 < 一圈 → true(无需淘汰)
    REQUIRE(rb.evictSlowConsumer(0));
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑧ 边界
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ringbuffer: readRecent rejects out==nullptr, count==0, invalid consumerId",
          "[master][datahub][ringbuffer][neg]") {
    RingBuffer<Sample> rb(32, "neg");
    rb.push(mkSample(1, 1, 0.0f));
    Sample out[1];

    REQUIRE(rb.readRecent(0, nullptr, 1) == 0u);       // 空指针
    REQUIRE(rb.readRecent(0, out, 0) == 0u);           // 零长度
    REQUIRE(rb.readRecent(-1, out, 1) == 0u);          // cid < 0
    REQUIRE(rb.readRecent(4, out, 1) == 0u);           // cid >= MAX_CONSUMERS(4)
}

TEST_CASE("ringbuffer: capacity() reflects constructor argument",
          "[master][datahub][ringbuffer][cap-api]") {
    REQUIRE(RingBuffer<Sample>(64).capacity() == 64u);
    REQUIRE(RingBuffer<Sample>(1024).capacity() == 1024u);
}
