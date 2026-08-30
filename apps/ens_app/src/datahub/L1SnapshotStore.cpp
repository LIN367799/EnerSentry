// src/datahub/L1SnapshotStore.cpp
// L3 数据中枢 ── L1 快照库实现（ENS-LLD-200 §3.4/3.4.1）。
//
// 实现要点：
//   * initFromPolicy：扫一遍算 minId/maxId；range ≤ size*2 走稠密 vector，否则 QHash
//   * capacityForPolicy：向上取 2 幂（nextPow2），钳制 [256, 65536]
//   * lookup：inline 数组下标优先 → miss 走 QHash（热路径零哈希）

#include "L1SnapshotStore.h"

#include <algorithm>
#include <climits>
#include <cstdint>

namespace ens::datahub {

namespace {

/// 向上取 2 的幂（标准 bit-twiddling）
constexpr size_t nextPow2(size_t v) noexcept {
    if (v <= 1) return 1;
    v--;
    for (size_t i = 1; i < sizeof(size_t) * CHAR_BIT; i <<= 1) v |= v >> i;
    return v + 1;
}

}  // namespace

L1SnapshotStore::~L1SnapshotStore() = default;

size_t L1SnapshotStore::capacityForPolicy(const RingBufferPolicyEntry& e) {
    if (e.sampleRateMs == 0) return 256;                              // 防除零兜底
    // 槽数 = 保留时长 / 采样周期 + 1（保险余量）
    size_t slots = static_cast<size_t>(e.retentionMs / e.sampleRateMs) + 1;
    slots = nextPow2(slots);
    return std::clamp(slots, size_t{256}, size_t{65536});
}

bool L1SnapshotStore::initFromPolicy(const QVector<RingBufferPolicyEntry>& policy) {
    if (policy.isEmpty()) return false;

    // 清空旧 state（unique_ptr 析构自动释放 RingBuffer）
    m_bufferByIndex.clear();
    m_sparseBuffers.clear();

    // 1. 计算 ID 范围与稠密度
    uint32_t minId = UINT32_MAX, maxId = 0;
    for (const auto& e : policy) {
        minId = std::min(minId, e.pointId);
        maxId = std::max(maxId, e.pointId);
    }
    const uint32_t range = static_cast<uint32_t>(maxId - minId + 1);
    const bool dense = (range <= static_cast<uint32_t>(policy.size() * 2));

    // 2. 稠密场景:预分配 vector 默认构造(空 unique_ptr 槽位占位;assign(fill) 对 move-only 失败)
    if (dense) {
        m_minPointId = minId;
        m_maxPointId = maxId;
        m_bufferByIndex.resize(range);
    }

    // 3. 为每个策略条目创建 RingBuffer
    //    RingBuffer 构造时 std::vector<std::atomic<T>>(capacity) 已分配
    for (const auto& e : policy) {
        const size_t cap = capacityForPolicy(e);
        // 命名约定：name 用于 qWarning 诊断（slow_consumer_evicted）
        auto rb = std::make_unique<RingBuffer<Sample>>(
            cap, QString("pid=%1 cap=%2").arg(e.pointId).arg(cap));
        if (dense) {
            m_bufferByIndex[e.pointId - minId] = std::move(rb);
        } else {
            m_sparseBuffers[e.pointId] = std::move(rb);   // unordered_map operator[] 支持 move-only
        }
    }
    return true;
}

size_t L1SnapshotStore::registeredCount() const noexcept {
    size_t n = 0;
    for (const auto& up : m_bufferByIndex) if (up) ++n;
    return n + m_sparseBuffers.size();
}

}  // namespace ens::datahub
