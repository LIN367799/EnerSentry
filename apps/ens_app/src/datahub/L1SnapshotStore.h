// src/datahub/L1SnapshotStore.h
// L3 数据中枢 ── L1 快照库（稠密 ID 数组 + QHash 稀疏回退）（ENS-LLD-200 §3.4/3.4.1）。
//
// 设计目标：消除"采集线程每次 write 走 QHash 查找"的 CPU 热点。
//   * 稠密连续 ID 范围 → std::vector<RingBuffer<Sample>*> 直接下标寻址（O(1)）
//   * 稀疏/大跨度 ID   → QHash<uint32_t, RingBuffer<Sample>*> 回退
//
// 与 LLD-200 §3.4 草案的差异（V.x 落地修订）：
//   * RingBuffer 持有改 unique_ptr<RingBuffer<Sample>>：原 LLD 用裸指针存在泄漏风险，
//     落到 std::unique_ptr 让 L1SnapshotStore 拥有生命周期，析构自动释放

#pragma once

#include "IL1SnapshotReader.h"
#include "RingBuffer.h"
#include "Sample.h"

#include <QMutex>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ens::datahub {

/// @brief 单测点 RingBuffer 容量策略（LLD-200 §3.5.2 NFR-PERF-05 表格）
struct RingBufferPolicyEntry {
    uint32_t pointId      = 0;
    uint32_t sampleRateMs = 100;     // 默认 100ms
    uint32_t retentionMs  = 3'600'000; // 默认 1h
    uint8_t  priority     = 1;       // 0=核心 1=普通 2=状态
};

/// @brief L1 快照库：按 pointId 路由到对应 RingBuffer，热路径 O(1) 数组下标
/// 切片 38：实现 IL1SnapshotReader（AlarmCenter 回放弹窗经抽象读取，FR-AL-12）
class L1SnapshotStore : public IL1SnapshotReader {
public:
    L1SnapshotStore() = default;
    ~L1SnapshotStore();

    L1SnapshotStore(const L1SnapshotStore&) = delete;
    L1SnapshotStore& operator=(const L1SnapshotStore&) = delete;
    L1SnapshotStore(L1SnapshotStore&&) = delete;
    L1SnapshotStore& operator=(L1SnapshotStore&&) = delete;

    /// 按点表策略初始化各测点 RingBuffer，自动选择数组或稀疏索引
    /// @return true 初始化成功（policy 非空）；false 失败
    /// @sideeffect 重新 init 会析构旧 RingBuffer
    bool initFromPolicy(const QVector<RingBufferPolicyEntry>& policy);

    /// 采集线程调用：写入单个测点（O(1) 无锁，数组下标优先）
    inline void write(uint32_t pointId, const Sample& s) noexcept {
        auto* rb = lookup(pointId);
        if (rb) rb->push(s);                                          // 未注册测点静默丢弃
    }

    /// UI / 降采样线程调用：读取最近 N 个
    size_t readRecent(uint32_t pointId, int consumerId,
                      Sample* out, size_t count) const noexcept {
        auto* rb = lookup(pointId);
        return (rb) ? rb->readRecent(consumerId, out, count) : 0;
    }

    /// 黑匣子原子提取（持锁仅拷贝，参见 LLD-200 §3.6）
    size_t extractRange(uint32_t pointId, uint64_t startTs, uint64_t endTs,
                        Sample* out, size_t maxCount) const noexcept {
        auto* rb = lookup(pointId);
        return (rb) ? rb->extractRange(startTs, endTs, out, maxCount) : 0;
    }

    // ── IL1SnapshotReader（切片 38，FR-AL-12 回放）──
    size_t replayExtract(uint32_t pointId, uint64_t beginMs, uint64_t endMs,
                         Sample* out, size_t maxCount) const noexcept override {
        return extractRange(pointId, beginMs, endMs, out, maxCount);
    }

    /// 黑匣子锁定槽位（防止滚动淘汰覆盖，V1.1 预留；本切片暂空）
    /// @design-intent 待切片 9 BlackBoxManager 收口时实现元数据保护
    void lockRange(uint32_t /*pointId*/, uint64_t /*startTs*/, uint64_t /*endTs*/) {
        // 暂空实现：占位 m_lockMutex 字段待切片 9 使用
    }

    /// 诊断查询：测点是否已注册
    bool hasPoint(uint32_t pointId) const noexcept {
        return lookup(pointId) != nullptr;
    }

    /// 诊断查询：已注册测点数量
    size_t registeredCount() const noexcept;

    /// 诊断查询：稠密 / 稀疏
    bool isDense() const noexcept { return !m_bufferByIndex.empty(); }

    /// 容量预算：按 sampleRateMs/retentionMs 计算槽数，向上取 2 的幂
    /// 钳制 [256, 65536]：256 防小测点浪费，65536 是 16-bit 位图上限
    /// @public 公开以便测试 + 配置层校验（pure function，无副作用）
    static size_t capacityForPolicy(const RingBufferPolicyEntry& e);

private:
    /// 热路径查找：优先 O(1) 数组下标，miss 再回退 unordered_map
    /// @note LLD-200 §3.4 草案用 QHash；Qt 5 QHash::insert 不支持 move-only value
    ///       （std::unique_ptr 不可拷贝），改 std::unordered_map 完美支持 + 性能可比
    inline RingBuffer<Sample>* lookup(uint32_t pointId) const noexcept {
        if (!m_bufferByIndex.empty() &&
            pointId >= m_minPointId && pointId <= m_maxPointId) {
            return m_bufferByIndex[pointId - m_minPointId].get();
        }
        auto it = m_sparseBuffers.find(pointId);
        return (it != m_sparseBuffers.end()) ? it->second.get() : nullptr;
    }

    // 稠密：vector 持 RingBuffer 所有权 + 下标寻址（m_bufferByIndex[i] 对应 pointId = m_minPointId + i）
    std::vector<std::unique_ptr<RingBuffer<Sample>>> m_bufferByIndex;
    uint32_t m_minPointId = 0;
    uint32_t m_maxPointId = 0;
    // 稀疏:std::unordered_map 持所有权 + 键寻址(替代 LLD-200 §3.4 草案的 QHash:
    // QHash::insert 在 Qt 5 上对 move-only value 仍走拷贝路径,改 unordered_map 完美支持)
    std::unordered_map<uint32_t, std::unique_ptr<RingBuffer<Sample>>> m_sparseBuffers;
    // 仅保护 lockRange 元数据（V1.1 预留）
    mutable QMutex m_lockMutex;
};

}  // namespace ens::datahub
