// tests/unit/test_databus.cpp
// L3 数据中枢 ── DataBus Tier 2 单测（DevGuide §4.1.3 + Phase 3 4.1.3）。
//
// 覆盖：
//   ① 订阅通配 push 10 收 10:单订阅者收满
//   ② 退订后不再收
//   ③ 多订阅者订阅同 pointId:各收全
//   ④ 多个不同 pointId 订阅:只收到匹配点
//   ⑤ 通配订阅:任意 pointId 广播都触发
//   ⑥ 订阅 nullptr 返 0;unsubscribe 不存在 handle 返 false
//   ⑦ subscriberCount / wildcardCount 统计

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <vector>

#include "datahub/DataBus.h"
#include "datahub/Sample.h"

using ens::datahub::DataBus;
using ens::datahub::IDataBusSubscriber;
using ens::datahub::Sample;
using ens::datahub::Subscription;

namespace {

// Mock 订阅者:count + lastSample
class MockSub : public IDataBusSubscriber {
public:
    std::atomic<int> count{0};
    Sample           lastSample{};

    void onSample(const Sample& s) noexcept override {
        ++count;
        lastSample = s;
    }
};

Sample mkSample(uint64_t ts, uint32_t pid, float v) {
    Sample s;
    s.timestamp = ts;
    s.pointId   = pid;
    s.value     = v;
    return s;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ① 订阅通配 push 10 收 10
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("databus: subscribe then broadcast delivers 10/10 to subscriber",
          "[master][datahub][databus][deliver]") {
    DataBus bus;
    MockSub sub;
    const Subscription h = bus.subscribe(/*pointId=*/42, &sub);
    REQUIRE(h != 0);

    for (int i = 0; i < 10; ++i) {
        bus.broadcast(mkSample(static_cast<uint64_t>(i + 1), 42, 0.0f));
    }
    REQUIRE(sub.count.load() == 10);
    REQUIRE(sub.lastSample.timestamp == 10u);
    REQUIRE(sub.lastSample.pointId   == 42u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ② 退订后不再收
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("databus: unsubscribe stops delivery",
          "[master][datahub][databus][unsub]") {
    DataBus bus;
    MockSub sub;
    const Subscription h = bus.subscribe(1, &sub);
    bus.broadcast(mkSample(1, 1, 0.0f));
    REQUIRE(sub.count.load() == 1);

    REQUIRE(bus.unsubscribe(h));
    bus.broadcast(mkSample(2, 1, 0.0f));
    REQUIRE(sub.count.load() == 1);                    // 退订后未增
}

// ─────────────────────────────────────────────────────────────────────────────
// ③ 多订阅者订阅同 pointId:各收全
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("databus: multiple subscribers on same pointId each receive all",
          "[master][datahub][databus][multi-sub]") {
    DataBus bus;
    MockSub a, b, c;
    bus.subscribe(7, &a);
    bus.subscribe(7, &b);
    bus.subscribe(7, &c);
    for (int i = 0; i < 5; ++i) {
        bus.broadcast(mkSample(static_cast<uint64_t>(i + 1), 7, 0.0f));
    }
    REQUIRE(a.count.load() == 5);
    REQUIRE(b.count.load() == 5);
    REQUIRE(c.count.load() == 5);
}

// ─────────────────────────────────────────────────────────────────────────────
// ④ 多个不同 pointId 订阅:只收匹配点
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("databus: per-pointId routing delivers only to matching subscribers",
          "[master][datahub][databus][routing]") {
    DataBus bus;
    MockSub subA, subB;
    bus.subscribe(100, &subA);
    bus.subscribe(200, &subB);
    bus.broadcast(mkSample(1, 100, 0.0f));
    bus.broadcast(mkSample(2, 200, 0.0f));
    bus.broadcast(mkSample(3, 100, 0.0f));
    bus.broadcast(mkSample(4, 999, 0.0f));           // 不匹配任何
    REQUIRE(subA.count.load() == 2);
    REQUIRE(subB.count.load() == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑤ 通配订阅
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("databus: wildcard subscription receives any pointId",
          "[master][datahub][databus][wildcard]") {
    DataBus bus;
    MockSub wild;
    MockSub specific;
    bus.subscribeWildcard(&wild);
    bus.subscribe(42, &specific);
    bus.broadcast(mkSample(1, 1, 0.0f));
    bus.broadcast(mkSample(2, 42, 0.0f));
    bus.broadcast(mkSample(3, 100, 0.0f));
    REQUIRE(wild.count.load() == 3);                   // 通配全收
    REQUIRE(specific.count.load() == 1);              // 只收 pid=42
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑥ 错误处理
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("databus: nullptr subscriber yields 0 handle; invalid unsubscribe returns false",
          "[master][datahub][databus][neg]") {
    DataBus bus;
    REQUIRE(bus.subscribe(1, nullptr) == 0);
    REQUIRE(bus.subscribeWildcard(nullptr) == 0);
    REQUIRE_FALSE(bus.unsubscribe(0));
    REQUIRE_FALSE(bus.unsubscribe(12345));             // 不存在
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑦ 计数
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("databus: subscriberCount / wildcardCount reflect registrations",
          "[master][datahub][databus][count]") {
    DataBus bus;
    REQUIRE(bus.subscriberCount() == 0u);
    REQUIRE(bus.wildcardCount() == 0u);

    MockSub a, b, c;
    bus.subscribe(1, &a);
    bus.subscribe(2, &b);
    bus.subscribeWildcard(&c);
    REQUIRE(bus.subscriberCount() == 3u);
    REQUIRE(bus.wildcardCount() == 1u);

    REQUIRE(bus.unsubscribe(bus.subscribe(3, &a)));    // 立即退订
    REQUIRE(bus.subscriberCount() == 3u);              // a 仍订阅了 pid=1/wildcard,且 3 已退 → 总数仍 3
}
