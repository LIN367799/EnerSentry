// src/datahub/RingBuffer.h
// L3 数据中枢 ── 无锁环形缓冲（SPSC 生产者 + 多消费者 Cursor）（ENS-LLD-200 §3.2/3.5.1）。
//
// 关键设计（LLD-200 §3.2/3.3 时序 + §3.5.1 慢消费者淘汰）：
//   * 容量 capacity 在构造时指定（运行时 2 幂 + 位掩码 m_mask）：支持 L1SnapshotStore
//     按点表策略分配不同容量（LLD §3.5.2 NFR-PERF-05 分级：100ms/1h→65536, 1s/30min→2048…）
//   * T 必须可 std::atomic 化且运行时 lock-free —— 强契约由调用方保证
//     （Sample 16B 对齐 + cmpxchg16b 真 lock-free on x86-64；MSVC 14.x
//      std::atomic<Sample>::is_always_lock_free constexpr 误报决策见
//      Sample.h "MSVC 14.x constexpr 保守补充" 段 — Tier 1 运行期兜底）
//   * 二级发布指针避免撕裂读：
//       m_writePos      → 已 fetch_add（数据可能未发布）— 消费者不可读
//       m_publishedPos  → 数据已完整可见 — 消费者可读上限（release/acquire 配对）
//   * 多消费者各自游标互不竞争：m_consumerCursors[4]
//   * 慢消费者强制跳跃：evictSlowConsumer 滞后 ≥ 一圈时跳到最老可读槽位
//
// 与 LLD-200 §3.2 草案的差异（V.x 落地修订）：
//   * 模板参数 Capacity 改为运行时构造参数 + 成员 m_mask：
//     原草案用非类型模板参数 Capacity，所有实例必须同容量，无法满足
//     L1SnapshotStore 按策略分配不同容量（§3.5.2 分级预算）；改运行时后位掩码
//     仍在 2 幂下走快速路径，性能差异 < 5%
//   * 移除 `static_assert(std::atomic<T>::is_always_lock_free, ...)`：MSVC 14.x
//     constexpr 误报对 Sample 会触发编译失败；改为运行期契约（调用方 Tier 1 保证）
//   * Slot 32B 增强结构（epoch/sequence）见 §3.5.3，仅极端场景启用，本类不内置
//   * extractRange 退化为 O(N) 时间窗扫描（LLD §3.2 描述 O(log N) 二分仅当 ts
//     "未覆盖一圈"单调递增；为安全实现按 published 段线性扫描，命中即返回）

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <QString>

namespace ens::datahub {

/// @brief 无锁环形缓冲：单生产者（采集线程）写入，多消费者（UI/黑匣子/降采样/预留）读取。
/// @tparam T 元素类型（必须 16B 对齐 + 运行时 lock-free atomic，如 ens::datahub::Sample）
/// @note 容量在构造时指定；必须 2 的幂（构造函数运行时断言 + 抛 std::invalid_argument）
template <typename T>
class RingBuffer {
public:
    /// 消费者槽位：[0]=UI [1]=黑匣子 [2]=降采样 [3]=预留
    static constexpr size_t MAX_CONSUMERS = 4;

    /// @param capacity 槽位数（必须 2 的幂；>= 2 且 <= 65536 防 16-bit 索引溢出）
    /// @throw std::invalid_argument 当 capacity 非 2 幂或越界
    explicit RingBuffer(size_t capacity, const QString& name = QString())
        : m_name(name),
          m_mask(capacity - 1),
          m_buffer(capacity) {
        if (capacity < 2 || capacity > 65536 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument(
                "RingBuffer capacity must be a power of 2 in [2, 65536]");
        }
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    // ════════════════════ 生产者侧（仅单生产者：采集线程）════════════════════

    /// 单元素写入（fetch_add + relaxed 位移 → 写数据 → release 屏障 + 发布）
    /// @note m_publishedPos 语义 = "已发布数据量"(counter);推 N 次后 = N(初始 0 表示无数据)
    void push(const T& item) noexcept {
        const size_t pos = m_writePos.fetch_add(1, std::memory_order_relaxed);
        const size_t idx = pos & m_mask;                              // 位掩码取模
        m_buffer[idx].store(item, std::memory_order_relaxed);          // ① 写数据
        std::atomic_thread_fence(std::memory_order_release);           // ② Store-Store 屏障
        m_publishedPos.store(pos + 1, std::memory_order_release);      // ③ 发布(数据量+1)
    }

    /// 批量写入（一次发布减少屏障次数）
    void pushBatch(const T* items, size_t count) noexcept {
        if (items == nullptr || count == 0) return;
        const size_t startPos = m_writePos.fetch_add(count, std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i) {
            const size_t idx = (startPos + i) & m_mask;
            m_buffer[idx].store(items[i], std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_release);
        m_publishedPos.store(startPos + count, std::memory_order_release);
    }

    // ════════════════════ 消费者侧（多消费者，独立游标）════════════════════

    /// 获取最新已发布位置（acquire 语义：与生产者 release 配对）
    size_t latestPublished() const noexcept {
        return m_publishedPos.load(std::memory_order_acquire);
    }

    /// 读取最近 N 个元素（消费者 id 隔离游标，互不竞争）
    /// @return 实际读取数量（无新数据返 0；消费者过慢自动跳到最老可读）
    /// @note counter 语义:published = 已发布数据量,cursor = 下一个待读位置
    ///       可读区间 [cursor, published) 长度 = published - cursor
    size_t readRecent(int consumerId, T* out, size_t count) noexcept {
        if (out == nullptr || count == 0) return 0;
        if (consumerId < 0 || consumerId >= static_cast<int>(MAX_CONSUMERS)) return 0;
        const size_t cap = m_buffer.size();
        if (count > cap) count = cap;

        const size_t published = m_publishedPos.load(std::memory_order_acquire);
        std::atomic<size_t>& cursorAtomic = m_consumerCursors[consumerId];
        size_t cursor = cursorAtomic.load(std::memory_order_relaxed);

        if (published <= cursor) return 0;                            // 无新数据(初态/已追上)
        // 慢消费者:cursor 已落到被覆盖区,跳到最老可读位置(max(0, published-cap))
        if (published - cursor >= cap) {
            cursor = published - cap;                                 // newCursor = max(0, p-cap)
        }

        const size_t readable = std::min(published - cursor, count);
        for (size_t i = 0; i < readable; ++i) {
            const size_t idx = (cursor + i) & m_mask;                 // 从 cursor 开始
            out[i] = m_buffer[idx].load(std::memory_order_acquire);   // 与 producer release 配对
        }
        cursorAtomic.store(cursor + readable, std::memory_order_release);
        return readable;
    }

    /// 按时间戳窗口原子提取（黑匣子场景）
    /// @design-intent published 段上线性扫描；遇 timestamp > endTs 即停
    /// @return 提取数量（按 ts 升序；startTs > endTs 或无数据返 0）
    /// @note counter 语义:有效区间 [0, published) 半开,长度 = published
    size_t extractRange(uint64_t startTs, uint64_t endTs,
                        T* out, size_t maxCount) const noexcept {
        if (out == nullptr || maxCount == 0 || startTs > endTs) return 0;
        const size_t published = m_publishedPos.load(std::memory_order_acquire);
        if (published == 0) return 0;                                  // 尚无数据

        size_t count = 0;
        for (size_t pos = 0; pos < published && count < maxCount; ++pos) {
            const size_t idx = pos & m_mask;
            const T val = m_buffer[idx].load(std::memory_order_acquire);
            if (val.timestamp >= startTs) {
                if (val.timestamp > endTs) break;                     // 超出右边界
                out[count++] = val;
            }
        }
        return count;
    }

    /// 慢消费者强制跳跃（滞后 ≥ 一圈时推进游标到最老可读槽位；LLD §3.5.1）
    /// @return true 游标正常（无需淘汰）；false 已跳跃淘汰
    /// @sideeffect 越过时 qWarning 日志（slow_consumer_evicted）
    /// @note counter 语义:最老可读位置 = max(0, published-cap);落后 ≥ cap 时淘汰
    bool evictSlowConsumer(int consumerId) noexcept {
        if (consumerId < 0 || consumerId >= static_cast<int>(MAX_CONSUMERS)) return true;
        const size_t cap = m_buffer.size();
        const size_t published = m_publishedPos.load(std::memory_order_acquire);
        std::atomic<size_t>& cursorAtomic = m_consumerCursors[consumerId];
        const size_t cursor = cursorAtomic.load(std::memory_order_relaxed);
        if (published - cursor >= cap) {                               // 滞后 ≥ cap
            const size_t newCursor = published - cap;                 // = max(0, p-cap)
            cursorAtomic.store(newCursor, std::memory_order_release);
            qWarning("slow_consumer_evicted cid=%d skipped=%zu published=%zu name=%s",
                     consumerId, newCursor - cursor, published,
                     m_name.toUtf8().constData());
            return false;
        }
        return true;
    }

    size_t capacity() const noexcept { return m_buffer.size(); }
    const QString& name() const noexcept { return m_name; }

private:
    QString m_name;
    const size_t m_mask;                                              // capacity-1（位掩码）
    std::vector<std::atomic<T>> m_buffer;                             // 数据槽位（atomic 读）
    std::atomic<size_t> m_writePos{0};                                // 已 fetch_add（数据可能未发布）
    std::atomic<size_t> m_publishedPos{0};                            // 已发布（消费者可读安全上限）
    std::array<std::atomic<size_t>, MAX_CONSUMERS> m_consumerCursors{}; // 每消费者独立游标
};

}  // namespace ens::datahub
