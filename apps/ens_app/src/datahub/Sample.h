// src/datahub/Sample.h
// L3 数据中枢 ── L1/L2 高频采样单元（ENS-LLD-200 §3.1 / ADR-08 / ADR-18）。
// 关键约束：
//   ① 16B 对齐 → x86-64 一条 movaps 完成整体读写，杜绝"撕裂读"；
//   ② 字段顺序保证 timestamp 落在 8 字节边界（packed-aware 平台兼容）；
//   ③ std::atomic<Sample>::is_always_lock_free 编译期断言，32-bit / ARMv7 上自动失败并强制改型。
//
// 数据契约（3.1.6 阶段固化，3.1.3 ModbusEngine 写、3.1.7 端到端均按此布局）：
//   ┌─────────────────────┬──────────────┬──────────┐
//   │ timestamp (uint64)  │ pointId(u32) │ value(f) │
//   │       8B           │     4B       │    4B    │
//   │ offset 0 ──────────► offset 8       │ offset 12│
//   └─────────────────────┴──────────────┴──────────┘
//                     合计 16 字节（无 padding）
//
// 后续 3.1.5 PointTable 落地后，pointId ↔ (slaveAddress, registerAddr, dataType, byteOrder,
// scaleFactor) 由 PointRuntime 关联；本头仅承载"已解析完的工程值",不与传输层耦合。

#pragma once

#include <atomic>
#include <cstdint>

// ── 跨平台 16 字节缓存行对齐宏（ENS-LLD-200 §3.1）──
#if defined(_MSC_VER)
    #define ENS_CACHE_ALIGN __declspec(align(16))
#elif defined(__GNUC__) || defined(__clang__)
    #define ENS_CACHE_ALIGN __attribute__((aligned(16)))
#else
    #define ENS_CACHE_ALIGN alignas(16)
#endif

namespace ens::datahub {

/// @brief L1 高频采样单元（热路径结构体，必须 16B 对齐）。
/// @design-intent 16 字节使 x86-64 单条 `movaps` 完成原子读写，
///                杜绝"撕裂读"；字段顺序保证 timestamp 落在 8 字节边界。
struct ENS_CACHE_ALIGN Sample {
    uint64_t timestamp;   // Unix 毫秒时间戳（8B，对齐到地址 8 的倍数）
    uint32_t pointId;     // 测点 ID（4B）
    float    value;       // 工程值（已应用 scaleFactor/byteOrder，4B）
};

// ── sizeof 强约束（ADR-08 / ADR-18）──
// 16 字节硬约束必须 100% 在编译期捕获：未来误增字段或改 padding 立即触发。
// 注：std::atomic<Sample>::is_always_lock_free *不*在此断言——
//   MSVC 14.x x86-64 constexpr 保守策略会误报 false（即便运行时 cmpxchg16b 是 lock-free），
//   GCC / Clang / Apple Silicon 上 constexpr 恒为 true；LLD-200 §3.1 "边界场景处理"段
//   的运行期兜底由 Tier 1 测试 test_sample.cpp::"sample: std::atomic<Sample> runtime
//   lock-free check" 强制执行。
static_assert(sizeof(Sample) == 16,
    "Sample must be 16 bytes for atomic store/load on x86-64");

}  // namespace ens::datahub
