// tests/unit/test_txid_allocator.cpp
// TransactionIdAllocator Tier 2 单测（ENS-DEV-GUIDE §2A 2.1.5）：耗尽/复用/清空。
// 用例名必须纯 ASCII（Catch2 v3 在 GBK 控制台解析 --list-tests JSON 的限制）。

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <vector>
#include "TransactionIdAllocator.h"

using ens::protocol::TransactionIdAllocator;

TEST_CASE("TxId: 65535 consecutive allocations yield 1..65535 distinct, 65536th returns 0", "[protocol][txid]") {
    TransactionIdAllocator alloc;
    std::vector<uint16_t> seen;
    seen.reserve(65535);
    for (int i = 0; i < 65535; ++i) {
        const uint16_t id = alloc.allocate();
        REQUIRE(id != TransactionIdAllocator::INVALID_ID);
        seen.push_back(id);
    }
    std::sort(seen.begin(), seen.end());                    // 排序后查相邻重复（O(n log n)）
    REQUIRE(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
    REQUIRE(seen.front() == 1);
    REQUIRE(seen.back() == 65535);
    REQUIRE(alloc.allocate() == TransactionIdAllocator::INVALID_ID);
}

TEST_CASE("TxId: repeated allocate after exhaustion returns INVALID_ID, no hang", "[protocol][txid]") {
    TransactionIdAllocator alloc;
    for (int i = 0; i < 65535; ++i) (void)alloc.allocate();
    REQUIRE(alloc.allocate() == TransactionIdAllocator::INVALID_ID);
    REQUIRE(alloc.allocate() == TransactionIdAllocator::INVALID_ID);  // 回归：曾死循环
}

TEST_CASE("TxId: released id is reusable (lowest-free)", "[protocol][txid]") {
    TransactionIdAllocator alloc;
    const uint16_t a = alloc.allocate();  // 1
    const uint16_t b = alloc.allocate();  // 2
    const uint16_t c = alloc.allocate();  // 3
    REQUIRE(a == 1);
    REQUIRE(b == 2);
    REQUIRE(c == 3);

    alloc.release(b);
    REQUIRE_FALSE(alloc.isAllocated(b));

    const uint16_t d = alloc.allocate();  // 最低空闲 = 2
    REQUIRE(d == 2);
    REQUIRE(alloc.isAllocated(d));
}

TEST_CASE("TxId: clearInFlight clears all, allocation restarts", "[protocol][txid]") {
    TransactionIdAllocator alloc;
    (void)alloc.allocate();
    (void)alloc.allocate();
    (void)alloc.allocate();
    REQUIRE(alloc.isAllocated(1));
    REQUIRE(alloc.isAllocated(3));

    alloc.clearInFlight();
    REQUIRE_FALSE(alloc.isAllocated(1));
    REQUIRE_FALSE(alloc.isAllocated(2));
    REQUIRE_FALSE(alloc.isAllocated(3));

    REQUIRE(alloc.allocate() == 1);
}
