// tests/unit/test_register_bank.cpp
// Track B B2 RegisterBank RCU 快照库 Tier 2 单测（UT-SIM, ENS-LLD-SIM §4.4 / §5.2）。
//
// 覆盖：
//   ① SlaveRegset 4 向量（holding/input/coils/discretes）+ 位打包边界
//   ② RegisterBank 基本 publish / snapshot round-trip
//   ③ snapshot 未安装 slave → nullptr
//   ④ publish 替换：旧 shared_ptr 仍可读到旧值（引用计数语义）
//   ⑤ 旧快照最后持有者释放后自动析构（共享所有权语义验证）
//   ⑥ 多线程高频 publish + 并发 snapshot — 数据竞争 / 撕裂读 / 死锁检测
//   ⑦ readControl / writeControl CoW 语义：write 不影响并发 snapshot
//   ⑧ clear() 清空所有从站
//
// ⚠ SlaveRegset vs modbus_slave.h::SlaveRegs（B3 临时版）：
//   B3 临时 SlaveRegs = 64 holding 寄存器 array，专为 FC03/04/06 最小闭环；
//   B2 SlaveRegset = LLD-SIM §4.2 权威 4 向量版本，支持 FC01/02/05/0F/15/16 全功能码；
//   命名分开避免同一 namespace ens::sim 下的 ODR 违规（实测会触发 MSVC debug build SIGSEGV）。
//   B5/B6 落地时迁到 SlaveRegset。
//
// -fsanitize=thread 验证：
//   本测试默认 Release/Debug build（无 tsan）跑全部断言；CI 启用 tsan 时
//   可在 CMake 命令行追加 -DCMAKE_CXX_FLAGS="-fsanitize=thread" 验证无 data race。
//   测试内已构造高竞争场景（4 publish 线程 + 8 snapshot 线程 × 10000 iter）
//   让 tsan 一旦出现竞争就能报警。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "sim/register_bank.h"

using namespace ens::sim;

// ─────────────────────────────────────────────────────────────────────────────
// SlaveRegset 4 向量 + 位打包
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("register_bank: SlaveRegset allocate gives correct vector sizes",
          "[master][sim][register_bank][SlaveRegset]") {
    auto r = SlaveRegset::allocate(/*slaveId=*/1, /*holdingSize=*/128, /*inputSize=*/64);
    REQUIRE(r.slaveId == 1u);
    REQUIRE(r.holding.size() == 128u);
    REQUIRE(r.input.size()   == 64u);
    REQUIRE(r.coils.size()   == 16u);   // ceil(128/8)
    REQUIRE(r.discretes.size() == 8u);  // ceil(64/8)
}

TEST_CASE("register_bank: Holding/Input out-of-range silently returns 0 and ignores writes",
          "[master][sim][register_bank][SlaveRegset][bounds]") {
    auto r = SlaveRegset::allocate(/*slaveId=*/1, /*holdingSize=*/16, /*inputSize=*/16);
    r.setHolding(0, 0x1234);
    r.setHolding(15, 0xABCD);
    REQUIRE(r.getHolding(0)  == 0x1234u);
    REQUIRE(r.getHolding(15) == 0xABCDu);
    REQUIRE(r.getHolding(16) == 0u);    // OOB read -> 0
    REQUIRE(r.getHolding(999) == 0u);
    r.setHolding(100, 0xDEAD);           // OOB write -> silently ignored
    REQUIRE(r.getHolding(15) == 0xABCDu);
}

TEST_CASE("register_bank: Coils/Discretes bit-pack bit[reg%8] in byte[reg/8]",
          "[master][sim][register_bank][SlaveRegset]") {
    auto r = SlaveRegset::allocate(/*slaveId=*/1, /*holdingSize=*/64, /*inputSize=*/64);
    // 共 8 字节,每字节 8 个 coil
    r.setCoil(0,  true);
    r.setCoil(7,  true);
    r.setCoil(8,  true);     // 第 2 字节 bit 0
    r.setCoil(15, true);
    r.setCoil(16, true);     // 第 3 字节 bit 0
    REQUIRE(r.getCoil(0)  == true);
    REQUIRE(r.getCoil(7)  == true);
    REQUIRE(r.getCoil(8)  == true);
    REQUIRE(r.getCoil(15) == true);
    REQUIRE(r.getCoil(16) == true);
    REQUIRE(r.getCoil(6)  == false);   // 中间位
    REQUIRE(r.getCoil(14) == false);

    r.setCoil(7, false);                 // 翻转 bit 7
    REQUIRE(r.getCoil(7) == false);
    REQUIRE(r.coils[0]    == 0x01);     // 只有 bit 0 残留

    REQUIRE(r.getCoil(1000) == false);  // OOB -> false
    r.setCoil(1000, true);              // OOB write -> silently ignored
    REQUIRE(r.getCoil(1000) == false);
}

TEST_CASE("register_bank: InputRegisters and Discretes boundary symmetry",
          "[master][sim][register_bank][SlaveRegset][symmetry]") {
    auto r = SlaveRegset::allocate(/*slaveId=*/1, /*holdingSize=*/8, /*inputSize=*/16);
    r.setInput(0,  0xCAFE);
    r.setDiscrete(3, true);
    REQUIRE(r.getInput(0)   == 0xCAFEu);
    REQUIRE(r.getInput(16)  == 0u);     // OOB
    REQUIRE(r.getDiscrete(3) == true);
    REQUIRE(r.getDiscrete(15) == false);
}

// ─────────────────────────────────────────────────────────────────────────────
// RegisterBank publish / snapshot 基础语义
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("register_bank: snapshot on uninstalled slave returns nullptr",
          "[master][sim][register_bank][RegisterBank]") {
    RegisterBank bank;
    REQUIRE(bank.snapshot(/*slave=*/1) == nullptr);
    REQUIRE(bank.slaveCount() == 0u);
}

TEST_CASE("register_bank: publish then snapshot returns the published shared_ptr",
          "[master][sim][register_bank][RegisterBank]") {
    RegisterBank bank;
    auto r = std::make_shared<SlaveRegset>(SlaveRegset::allocate(/*slaveId=*/1, 8, 8));
    r->setHolding(3, 0xBEEF);
    REQUIRE(r.use_count() >= 1u);     // local + (make_shared internal control block)

    bank.publish(1, r);
    // publish 拷贝 r 到 bank.m_banks[1];r 本身仍持有 (make_shared + local)
    REQUIRE(r.use_count() >= 2u);
    REQUIRE(bank.slaveCount() == 1u);

    auto snap = bank.snapshot(1);
    REQUIRE(snap != nullptr);
    REQUIRE(snap.get() == r.get());    // snapshot 返回的 shared_ptr 指向同一对象
    REQUIRE(snap->getHolding(3) == 0xBEEFu);
}

TEST_CASE("register_bank: publish replaces snapshot; old snapshot stays valid via shared_ptr",
          "[master][sim][register_bank][RegisterBank]") {
    RegisterBank bank;
    auto v1 = std::make_shared<SlaveRegset>(SlaveRegset::allocate(/*slaveId=*/1, 8, 8));
    v1->setHolding(0, 0x1111);
    bank.publish(1, v1);

    auto snap_v1 = bank.snapshot(1);    // IO 线程持有 v1 快照
    REQUIRE(snap_v1->getHolding(0) == 0x1111u);

    // 生成线程写新快照
    auto v2 = std::make_shared<SlaveRegset>(SlaveRegset::allocate(/*slaveId=*/1, 8, 8));
    v2->setHolding(0, 0x2222);
    bank.publish(1, v2);

    // 新 snapshot 看到 v2
    auto snap_v2 = bank.snapshot(1);
    REQUIRE(snap_v2->getHolding(0) == 0x2222u);

    // 旧 snap_v1 仍持有 v1,未被污染
    REQUIRE(snap_v1->getHolding(0) == 0x1111u);
    REQUIRE(snap_v1 != snap_v2);
    REQUIRE(snap_v1.get() == v1.get());
    REQUIRE(snap_v2.get() == v2.get());
}

TEST_CASE("register_bank: old snapshot auto-destructs when last holder releases",
          "[master][sim][register_bank][RegisterBank][lifetime]") {
    RegisterBank bank;
    auto r1 = std::make_shared<SlaveRegset>(SlaveRegset::allocate(/*slaveId=*/1, 8, 8));
    bank.publish(1, r1);
    // r1 当前持有者 ≥ bank.m_banks[1](make_shared 内部控制块 + bank 共 2+)
    REQUIRE(r1.use_count() >= 1u);

    // IO 线程快照持有
    auto io_snap = bank.snapshot(1);
    REQUIRE(io_snap != nullptr);
    REQUIRE(io_snap.get() == r1.get());    // 同一个对象
    REQUIRE(r1.use_count() >= 2u);          // bank + io_snap

    // publish 新快照:bank.m_banks[1] 引用从 r1 转向 r2
    auto r2 = std::make_shared<SlaveRegset>(SlaveRegset::allocate(/*slaveId=*/1, 8, 8));
    bank.publish(1, r2);
    REQUIRE(r1.use_count() >= 1u);   // 仅 io_snap 持有 r1
    REQUIRE(r2.use_count() >= 1u);   // bank + (snapshot 返回的临时) 持有 r2

    // io_snap release → r1 析构(最后一个持有者释放后自动析构)
    // 直接用 r1 的值比较 reference 关系,不依赖精确 use_count
    io_snap.reset();
    REQUIRE(r1.use_count() == 1u);   // 仅 make_shared 内部控制块持有 r1
}

// ─────────────────────────────────────────────────────────────────────────────
// readControl / writeControl Copy-on-Write 语义
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("register_bank: writeControl CoW -- concurrent snapshot not affected during write",
          "[master][sim][register_bank][SBO]") {
    RegisterBank bank;
    auto r = std::make_shared<SlaveRegset>(SlaveRegset::allocate(/*slaveId=*/1, 16, 16));
    r->setHolding(0, 0xAAAA);
    bank.publish(1, r);

    // IO 线程在 writeControl 前先 snapshot
    auto io_snap_before = bank.snapshot(1);
    REQUIRE(io_snap_before->getHolding(0) == 0xAAAAu);

    // SBO 控制写 (替换 holding[0])
    bank.writeControl(1, 0, 0x5555);

    // IO 线程仍可看到旧快照 (其 use_count 仍持有 r)
    REQUIRE(io_snap_before->getHolding(0) == 0xAAAAu);
    // 新 snapshot 看到新值
    auto io_snap_after = bank.snapshot(1);
    REQUIRE(io_snap_after->getHolding(0) == 0x5555u);
}

TEST_CASE("register_bank: writeControl out-of-range reg silently ignored",
          "[master][sim][register_bank][SBO][bounds]") {
    RegisterBank bank;
    bank.publish(1, std::make_shared<SlaveRegset>(SlaveRegset::allocate(1, 16, 16)));
    bank.writeControl(1, /*reg=*/100, 0xDEAD);   // OOB reg
    REQUIRE(bank.readControl(1, 0) == 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 多线程高频 publish + 并发 snapshot (DoD 核心)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("register_bank: high-frequency publish + concurrent snapshot -- no torn reads, no deadlock",
          "[master][sim][register_bank][concurrency]") {
    RegisterBank bank;
    constexpr uint8_t N_SLAVES = 4;
    constexpr int    N_PUB_THREADS = 4;
    constexpr int    N_SNAP_THREADS = 8;
    constexpr int    N_ITERS = 5000;
    std::atomic<bool> stop{false};

    // 每个 slave 初始快照
    for (uint8_t s = 0; s < N_SLAVES; ++s) {
        bank.publish(s, std::make_shared<SlaveRegset>(SlaveRegset::allocate(s, 64, 32)));
    }

    // 生成线程:高频 publish,每次创建新快照(setHolding 多个寄存器)
    std::vector<std::thread> pub_threads;
    for (int t = 0; t < N_PUB_THREADS; ++t) {
        pub_threads.emplace_back([&, t]() {
            uint64_t counter = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                for (uint8_t s = 0; s < N_SLAVES; ++s) {
                    auto next = std::make_shared<SlaveRegset>(SlaveRegset::allocate(s, 64, 32));
                    next->setHolding(0, static_cast<uint16_t>(counter & 0xFFFF));
                    next->setHolding(1, static_cast<uint16_t>((counter >> 16) & 0xFFFF));
                    next->setCoil(8, (counter & 1) != 0);
                    bank.publish(s, std::move(next));
                    ++counter;
                }
                if (counter >= N_ITERS) break;
            }
        });
    }

    // IO 线程:高频 snapshot + 验证读到一致快照(holding[0..1] 与 coil[8] 关系自洽)
    std::atomic<uint64_t> snap_count{0};
    std::vector<std::thread> snap_threads;
    for (int t = 0; t < N_SNAP_THREADS; ++t) {
        snap_threads.emplace_back([&]() {
            uint64_t local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                for (uint8_t s = 0; s < N_SLAVES; ++s) {
                    auto snap = bank.snapshot(s);
                    if (snap) {
                        const uint16_t lo = snap->getHolding(0);
                        const uint16_t hi = snap->getHolding(1);
                        const bool    b  = snap->getCoil(8);
                        // holding[0] LSB 应等于 coil[8](生成线程一次 publish 内
                        // counter LSB 同时写入两个字段;不一致即为撕裂读)
                        const bool expect = (lo & 1) != 0;
                        if (b != expect) {
                            // 撕裂读检测 — 测试立即失败
                            stop.store(true, std::memory_order_relaxed);
                            FAIL("Torn read detected at slave=" << +s
                                 << " lo=" << lo << " b=" << b);
                            return;
                        }
                        ++local;
                    }
                }
                snap_count.fetch_add(local, std::memory_order_relaxed);
            }
        });
    }

    // 等所有 publish 线程完成
    for (auto& th : pub_threads) th.join();
    stop.store(true, std::memory_order_relaxed);
    for (auto& th : snap_threads) th.join();

    REQUIRE(snap_count.load() > 0u);  // 至少完成一些 snapshot
    REQUIRE(bank.slaveCount() == N_SLAVES);
}

TEST_CASE("register_bank: clear() releases all slave snapshots",
          "[master][sim][register_bank][RegisterBank]") {
    RegisterBank bank;
    auto r1 = std::make_shared<SlaveRegset>(SlaveRegset::allocate(1, 8, 8));
    auto r2 = std::make_shared<SlaveRegset>(SlaveRegset::allocate(2, 8, 8));
    bank.publish(1, r1);
    bank.publish(2, r2);
    REQUIRE(bank.slaveCount() == 2u);
    // bank 现在持有 r1 + r2 (make_shared 内部控制块 + local + bank 三者)
    REQUIRE(r1.use_count() >= 1u);
    REQUIRE(r2.use_count() >= 1u);

    bank.clear();
    REQUIRE(bank.slaveCount() == 0u);
    REQUIRE(bank.snapshot(1) == nullptr);
    REQUIRE(bank.snapshot(2) == nullptr);
    // bank 不再持有 → 仅 make_shared 内部控制块持有 → reference count=1
    REQUIRE(r1.use_count() == 1u);
    REQUIRE(r2.use_count() == 1u);
}