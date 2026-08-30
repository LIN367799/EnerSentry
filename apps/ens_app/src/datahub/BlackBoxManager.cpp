// src/datahub/BlackBoxManager.cpp
// L3 数据中枢 ── 黑匣子快照管理器实现（ENS-LLD-200 §3.6.1）。

#include "BlackBoxManager.h"

#include <cstdint>
#include <utility>
#include <vector>

#include <QObject>

namespace ens::datahub {

BlackBoxManager::BlackBoxManager(L1SnapshotStore* l1Store)
    : m_l1Store(l1Store) {}
bool BlackBoxManager::enableCriticalSwap(const QString& swapPath) {
    auto swap = std::make_unique<CriticalSwapFile>();
    if (!swap->open(swapPath)) {
        qWarning("BlackBoxManager: critical swap open failed: %s → degraded",
                 qUtf8Printable(swapPath));
        return false;
    }
    m_swap = std::move(swap);
    return true;
}

BlackBoxSnapshot BlackBoxManager::triggerBlackBox(uint32_t pointId,
                                                  uint64_t alarmTime,
                                                  AlarmLevel level) {
    BlackBoxSnapshot snap;
    snap.pointId   = pointId;
    snap.alarmTime = alarmTime;
    snap.level     = level;

    // ════ 第 1 步:原子快照预拷贝(持锁 ~10μs) ════
    {
        // 锁仅保护 extractRange 读取窗口,防 L1 回卷覆盖;释放后 L1 恢复自由写入
        std::lock_guard<QMutex> lock(m_snapshotMutex);
        if (m_l1Store != nullptr) {
            snap.samples.resize(kMaxSamples);
            const size_t count = m_l1Store->extractRange(
                pointId,
                (alarmTime > kPreWindowMs) ? (alarmTime - kPreWindowMs) : 0,
                alarmTime + kPostWindowMs,
                snap.samples.data(), kMaxSamples);
            snap.samples.resize(count);
        }
    }   // ← 锁释放(RAII)

    // ════ 第 2 步:Critical 级 → mmap 即时落盘(进程崩溃不丢) ════
    if (level == AlarmLevel::Critical) {
        m_criticalTriggers.fetch_add(1, std::memory_order_relaxed);
        if (m_swap != nullptr) {
            m_swap->appendSnapshot(snap);      // memcpy ~50μs,异步落盘
        }
    }

    // 非 Critical / JSON 序列化 + L2 异步写入:属切片 10 业务层(AlarmEngine),此处仅快照
    return snap;
}

uint32_t BlackBoxManager::criticalSnapshotCount() const {
    return (m_swap != nullptr) ? m_swap->snapshotCount() : 0;
}

void BlackBoxManager::forceSync() {
    if (m_swap != nullptr) {
        // close() 内部 flushSync(0, totalFileSize) + 释放 mmap 句柄,让 reread 可重新 open
        m_swap->close();
    }
}

}  // namespace ens::datahub
