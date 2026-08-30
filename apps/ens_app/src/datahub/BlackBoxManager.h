// src/datahub/BlackBoxManager.h
// L3 数据中枢 ── 黑匣子快照管理器（ENS-LLD-200 §3.6.1 / ADR-14 / HLD §3.2.2）。
//
// 设计：
//   * triggerBlackBox:原子预拷贝(extractRange ±30s,持锁 <10μs)→ 立即释放 L1 → 异步慢处理
//   * Critical 级 → CriticalSwapFile mmap 即时落盘(断电安全)
//   * 非 Critical → 仅计数(JSON 序列化 + L2 异步写入属切片 10 业务层)

#pragma once

#include "CriticalSwapFile.h"
#include "L1SnapshotStore.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include <QObject>
#include <QMutex>

namespace ens::datahub {

class BlackBoxManager {
public:
    static constexpr uint64_t kPreWindowMs  = 30000;   // 告警前 30s
    static constexpr uint64_t kPostWindowMs = 30000;   // 告警后 30s
    static constexpr size_t   kMaxSamples   = 1200;    // ±30s @100ms(预分配)

    /// @param l1Store 非 own,L1SnapshotStore::extractRange 提供 ±30s 数据
    /// @note 不需要 QObject 父对象(triggerBlackBox 是同步方法,无信号槽)
    explicit BlackBoxManager(L1SnapshotStore* l1Store);
    ~BlackBoxManager() = default;

    BlackBoxManager(const BlackBoxManager&) = delete;
    BlackBoxManager& operator=(const BlackBoxManager&) = delete;

    /// 触发黑匣子快照
    ///   第 1 步:原子预拷贝(m_snapshotMutex 持锁 ~10μs;extractRange 后立即释放 L1)
    ///   第 2 步:Critical 级 → mmap 即时落盘(进程崩溃不丢)
    /// @return 快照副本(调用方/测试可检视)
    BlackBoxSnapshot triggerBlackBox(uint32_t pointId, uint64_t alarmTime,
                                     AlarmLevel level);

    /// 启用 Critical 级 mmap 落盘(启动时调用;失败则黑匣子降级,采样继续)
    /// @return true mmap 可用
    bool enableCriticalSwap(const QString& swapPath);

    /// 诊断:当前 mmap 已写快照数
    uint32_t criticalSnapshotCount() const;

    /// 诊断:Critical 级已触发次数(含 mmap 未启用时的降级计数)
    uint32_t criticalTriggerCount() const noexcept { return m_criticalTriggers.load(); }

    /// 诊断:mmap 是否可用
    bool criticalSwapEnabled() const noexcept { return m_swap != nullptr; }

private:
    L1SnapshotStore*            m_l1Store;     // 非 own
    std::unique_ptr<CriticalSwapFile> m_swap;  // Critical mmap(可选)
    QMutex                      m_snapshotMutex;   // 保护 extractRange 预拷贝(极短)
    std::atomic<uint32_t>       m_criticalTriggers{0};
};

}  // namespace ens::datahub
