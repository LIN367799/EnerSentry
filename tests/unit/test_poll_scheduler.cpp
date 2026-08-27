// tests/unit/test_poll_scheduler.cpp
// Tier 2 PollScheduler 单测 ── 切片 5 3.1.4 DoD：
//   * mock「总超时」从机 -> getNextPollDelayMs 阶梯升到 30s
//   * slaveIsolated 触发 (Tier 2)
//   * 半双工 FIFO 串行 / 全双工独立并发
//   * BMS 100ms 插队 / SBO 风暴防护
//   * HEALTHY -> DEGRADED (3 failures) -> ISOLATED (8 failures) -> PROBING (30s later)
//   * 任意成功响应立即恢复 HEALTHY
//   * PROBING 失败回 ISOLATED 不重复发 slaveIsolated(P2-8)

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdint>
#include <chrono>
#include <thread>

#include "protocol/PollScheduler.h"

using namespace ens::protocol;
using Catch::Approx;

// 时间推进用 ps.advanceClock() 显式驱动(生产 nowMs() 走 steady_clock)

TEST_CASE("poll_scheduler: enqueue/dequeue by priority + half-duplex FIFO serial",
          "[master][protocol][poll_scheduler][priority]") {
    PollScheduler ps;
    const uint8_t linkId = 1;
    // 设为全双工(TCP),以便在测试中连续多次 dequeueNext;半双工验证见单独 test
    LinkParams params;
    params.isHalfDuplex = false;
    ps.setLinkParams(linkId, params);

    PollTask t1 = PollTask::normal(/*slaveId=*/1, /*linkId=*/linkId, /*intervalMs=*/100);
    PollTask t2 = PollTask::normal(/*slaveId=*/2, /*linkId=*/linkId, /*intervalMs=*/100);
    PollTask t3 = PollTask::controlWrite(/*slaveId=*/1, /*linkId=*/linkId);
    PollTask t4 = PollTask::low(/*slaveId=*/22, /*linkId=*/linkId, /*intervalMs=*/1000);

    ps.enqueue(t4);
    ps.enqueue(t1);
    ps.enqueue(t2);
    ps.enqueue(t3);   // SBO 高优先级应抢占队首

    // 第一个应该是 SBO controlWrite (HIGH 抢占)
    auto first = ps.dequeueNext(linkId);
    REQUIRE(first.priority == PollPriority::High);
    REQUIRE(first.isControlCommand);

    // NORMAL 队列 FIFO
    auto n1 = ps.dequeueNext(linkId);
    REQUIRE(n1.priority == PollPriority::Normal);
    REQUIRE(n1.slaveId == 1);

    auto n2 = ps.dequeueNext(linkId);
    REQUIRE(n2.priority == PollPriority::Normal);
    REQUIRE(n2.slaveId == 2);

    // LOW 最后
    auto low = ps.dequeueNext(linkId);
    REQUIRE(low.priority == PollPriority::Low);
}

TEST_CASE("poll_scheduler: maxConsecutivePreempt starv-guard - 5x NORMAL then forced LOW",
          "[master][protocol][poll_scheduler][starvation]") {
    PollScheduler ps;
    const uint8_t linkId = 1;
    LinkParams params;
    params.maxConsecutivePreempt = 5;
    params.isHalfDuplex = false;       // 全双工:连续 dequeueNext 不被 busy 拦截
    ps.setLinkParams(linkId, params);

    // 队列:6 NORMAL + 1 LOW
    for (int i = 0; i < 6; ++i) {
        ps.enqueue(PollTask::normal(1, linkId, 100));
    }
    ps.enqueue(PollTask::low(22, linkId, 1000));

    // 前 5 次 dequeue 应全是 NORMAL
    for (int i = 0; i < 5; ++i) {
        auto t = ps.dequeueNext(linkId);
        REQUIRE(t.priority == PollPriority::Normal);
    }
    // 第 6 次饥饿保护 -> 强制 LOW 插播
    auto t = ps.dequeueNext(linkId);
    REQUIRE(t.priority == PollPriority::Low);
    REQUIRE(t.slaveId == 22);

    // 第 7 次回到 NORMAL
    auto after = ps.dequeueNext(linkId);
    REQUIRE(after.priority == PollPriority::Normal);
}

TEST_CASE("poll_scheduler: SBO storm-guard - 10x HIGH then forced NORMAL yield",
          "[master][protocol][poll_scheduler][sbo-storm]") {
    PollScheduler ps;
    const uint8_t linkId = 1;
    LinkParams params;
    params.maxConsecutiveSboBurst = 10;
    params.isHalfDuplex = false;
    ps.setLinkParams(linkId, params);

    // 队列:15 HIGH(SBO) + 1 NORMAL
    for (int i = 0; i < 15; ++i) {
        ps.enqueue(PollTask::controlWrite(1, linkId));
    }
    ps.enqueue(PollTask::normal(1, linkId, 100));

    // 前 10 次是 HIGH
    for (int i = 0; i < 10; ++i) {
        auto t = ps.dequeueNext(linkId);
        REQUIRE(t.priority == PollPriority::High);
    }
    // 第 11 次 -> 强制 NORMAL
    auto t = ps.dequeueNext(linkId);
    REQUIRE(t.priority == PollPriority::Normal);
}

TEST_CASE("poll_scheduler: getNextPollDelayMs tri-fuse - 3 fails DEGRADED, 8 fails ISOLATED 30000ms",
          "[master][protocol][poll_scheduler][DoD][Tier2][circuit-breaker]") {
    PollScheduler ps;
    const uint8_t sid = 17;     // PCS-01
    const uint8_t linkId = 1;
    ps.registerSlave(sid, linkId, /*originalIntervalMs=*/1000);

    // 初始 HEALTHY -> 1000ms
    REQUIRE(ps.healthOf(sid) == SlaveHealth::HEALTHY);
    REQUIRE(ps.getNextPollDelayMs(sid) == 1000);

    // 连续 3 次失败 -> DEGRADED × 3 = 3000ms
    for (int i = 0; i < 3; ++i) {
        ps.onResponseReceived(sid, /*success=*/false);
    }
    REQUIRE(ps.healthOf(sid) == SlaveHealth::DEGRADED);
    REQUIRE(ps.getNextPollDelayMs(sid) == 3000);

    // 继续失败 4~7 次 -> 仍 DEGRADED,delay 仍 ×3
    for (int i = 0; i < 4; ++i) {
        ps.onResponseReceived(sid, /*success=*/false);
    }
    REQUIRE(ps.healthOf(sid) == SlaveHealth::DEGRADED);
    REQUIRE(ps.getNextPollDelayMs(sid) == 3000);

    // 第 8 次失败 -> ISOLATED,delay 阶梯升到 30s
    ps.onResponseReceived(sid, /*success=*/false);
    REQUIRE(ps.healthOf(sid) == SlaveHealth::ISOLATED);
    REQUIRE(ps.getNextPollDelayMs(sid) == 30000);   // <- DoD 关键断言

    // 9+ 次失败 -> 仍 ISOLATED,delay 不再升
    for (int i = 0; i < 5; ++i) {
        ps.onResponseReceived(sid, /*success=*/false);
    }
    REQUIRE(ps.healthOf(sid) == SlaveHealth::ISOLATED);
    REQUIRE(ps.getNextPollDelayMs(sid) == 30000);
}

TEST_CASE("poll_scheduler: any 1 successful response immediately recovers HEALTHY",
          "[master][protocol][poll_scheduler][recovery]") {
    PollScheduler ps;
    const uint8_t sid = 1;
    ps.registerSlave(sid, /*linkId=*/1, 1000);

    // 推到 ISOLATED
    for (int i = 0; i < 8; ++i) ps.onResponseReceived(sid, false);
    REQUIRE(ps.healthOf(sid) == SlaveHealth::ISOLATED);

    // 任意 1 次成功 -> HEALTHY
    ps.onResponseReceived(sid, true);
    REQUIRE(ps.healthOf(sid) == SlaveHealth::HEALTHY);
    REQUIRE(ps.getNextPollDelayMs(sid) == 1000);
}

TEST_CASE("poll_scheduler: enterProbingIfDue after 30s -> PROBING issued immediately",
          "[master][protocol][poll_scheduler][probing]") {
    PollScheduler ps;
    const uint8_t sid = 1;
    ps.registerSlave(sid, /*linkId=*/1, 1000);
    for (int i = 0; i < 8; ++i) ps.onResponseReceived(sid, false);
    REQUIRE(ps.healthOf(sid) == SlaveHealth::ISOLATED);

    // 推进 30s 之后 -> enterProbingIfDue 触发
    ps.advanceClock(30000);
    ps.enterProbingIfDue(sid);
    REQUIRE(ps.healthOf(sid) == SlaveHealth::PROBING);
    REQUIRE(ps.getNextPollDelayMs(sid) == 0);   // PROBING 立即下发

    // 试探成功 -> HEALTHY
    ps.onResponseReceived(sid, true);
    REQUIRE(ps.healthOf(sid) == SlaveHealth::HEALTHY);
}

TEST_CASE("poll_scheduler: signal emission count - slaveDegraded / slaveIsolated each fires once",
          "[master][protocol][poll_scheduler][signals]") {
    PollScheduler ps;
    const uint8_t sid = 1;
    int degradedCount = 0;
    int isolatedCount = 0;
    int recoveredCount = 0;
    QObject::connect(&ps, &PollScheduler::slaveDegraded,
                     [&](uint8_t, int) { ++degradedCount; });
    QObject::connect(&ps, &PollScheduler::slaveIsolated,
                     [&](uint8_t, int) { ++isolatedCount; });
    QObject::connect(&ps, &PollScheduler::slaveRecovered,
                     [&](uint8_t) { ++recoveredCount; });

    ps.registerSlave(sid, 1, 1000);
    for (int i = 0; i < 8; ++i) ps.onResponseReceived(sid, false);
    REQUIRE(degradedCount == 1);
    REQUIRE(isolatedCount == 1);

    // 恢复
    ps.onResponseReceived(sid, true);
    REQUIRE(recoveredCount == 1);
}

TEST_CASE("poll_scheduler: probing failure back to ISOLATED - no duplicate slaveIsolated",
          "[master][protocol][poll_scheduler][probing][dedup]") {
    PollScheduler ps;
    const uint8_t sid = 1;
    int isolatedCount = 0;
    QObject::connect(&ps, &PollScheduler::slaveIsolated,
                     [&](uint8_t, int) { ++isolatedCount; });

    ps.registerSlave(sid, /*linkId=*/1, 1000);
    for (int i = 0; i < 8; ++i) ps.onResponseReceived(sid, false);
    REQUIRE(ps.healthOf(sid) == SlaveHealth::ISOLATED);
    REQUIRE(isolatedCount == 1);

    // 30s 后进入 PROBING
    ps.advanceClock(30000);
    ps.enterProbingIfDue(sid);
    REQUIRE(ps.healthOf(sid) == SlaveHealth::PROBING);

    // PROBING 试探失败 → 回 ISOLATED,但不再重复 emit slaveIsolated / 计数
    ps.onResponseReceived(sid, false);
    REQUIRE(ps.healthOf(sid) == SlaveHealth::ISOLATED);
    REQUIRE(isolatedCount == 1);
    REQUIRE(ps.isolatedCount() == 1);
}

TEST_CASE("poll_scheduler: timeoutCount cumulative",
          "[master][protocol][poll_scheduler][stats]") {
    PollScheduler ps;
    const uint8_t sid = 1;
    ps.registerSlave(sid, 1, 1000);
    REQUIRE(ps.timeoutCount() == 0);
    for (int i = 0; i < 5; ++i) ps.onResponseReceived(sid, false);
    REQUIRE(ps.timeoutCount() == 5);

    ps.onResponseReceived(sid, true);
    REQUIRE(ps.timeoutCount() == 5);   // 成功响应不清零 timeoutCount(只清 consecutiveFailures)
}

TEST_CASE("poll_scheduler: half-duplex busy flag - dequeueNext returns empty while frame in-flight",
          "[master][protocol][poll_scheduler][half-duplex]") {
    PollScheduler ps;
    const uint8_t linkId = 1;
    PollTask t1 = PollTask::normal(1, linkId, 100);
    PollTask t2 = PollTask::normal(2, linkId, 100);
    ps.enqueue(t1);
    ps.enqueue(t2);

    // 第一次出队:busy=false -> 正常出队 + busy=true
    auto first = ps.dequeueNext(linkId);
    REQUIRE(first.slaveId == 1);

    // 此时总线忙,再次出队应返空
    auto busyResult = ps.dequeueNext(linkId);
    REQUIRE_FALSE(busyResult.isValid());

    // 模拟响应到达 -> busy=false
    ps.onLinkFree(linkId);
    auto second = ps.dequeueNext(linkId);
    REQUIRE(second.isValid());
    REQUIRE(second.slaveId == 2);
}