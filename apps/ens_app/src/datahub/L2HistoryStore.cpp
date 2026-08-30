// src/datahub/L2HistoryStore.cpp
// L3 数据中枢 ── L2 历史持久化实现（ENS-LLD-200 §4.5）。

#include "L2HistoryStore.h"

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

#include <QHash>

namespace ens::datahub {

L2HistoryStore::L2HistoryStore(SQLiteDataAccess* dal, QObject* parent)
    : QObject(parent), m_dal(dal) {}

L2HistoryStore::~L2HistoryStore() {
    // 析构前尝试 flush(若调用方已先 flush 则是空操作)
    if (!m_writeBuffer.empty()) {
        flush();
    }
}

void L2HistoryStore::enqueueSample(const DownSampledSample& s) {
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    m_writeBuffer.push_back(s);
    // 背压:超 capacity 丢最老(LLD §4.5)
    if (m_writeBuffer.size() > m_capacity) {
        const size_t excess = m_writeBuffer.size() - m_capacity;
        m_writeBuffer.erase(m_writeBuffer.begin(),
                            m_writeBuffer.begin() + excess);
        m_droppedCount.fetch_add(excess, std::memory_order_relaxed);
        qWarning("L2HistoryStore: buffer overflow, dropped %zu oldest samples (backpressure)",
                 excess);
    }
}

bool L2HistoryStore::flush() {
    if (m_dal == nullptr) {
        qWarning("L2HistoryStore: flush called with null dal");
        return false;
    }
    // 双缓冲 swap:锁内仅做 O(1) swap,锁持有 < 0.1μs
    std::vector<DownSampledSample> batch;
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        if (m_writeBuffer.empty()) return true;
        batch.swap(m_writeBuffer);
    }

    // 按月分桶:用 dbPath 作为 key(同一月的同粒度表落在同一库)
    // 粒度 gran 不在 DownSampledSample 中(由 DownSampler 调 flush 时传入);
    // 本切片简化:使用 SQLiteDataAccess::getDatabasePath(ts)做月分桶,
    // 同月不同粒度走同一连接,粒度由 batchInsert 第二个参数决定。
    // 实际 LLD §4.5 描述按月独立事务,本类不持 gran — 调用方需在 enqueue 前指定。
    // 简化:把 gran 透传,这里用"占位"方案 — 调用方必须先设置 m_gran 或 L2HistoryStore
    // 需要包装(本切片先按 1s 粒度落库,5s/1m 上层另起 store)
    // ——见 V.x 修订:本切片只支持单粒度(默认 Gran1s),多粒度需多 store 实例

    const HistoryGranularity gran = HistoryGranularity::Gran1s;   // V.x:暂硬编码 1s

    QHash<QString, std::vector<DownSampledSample>> buckets;
    for (const auto& s : batch) {
        const QString dbPath = m_dal->getDatabasePath(s.timestamp);
        buckets[dbPath].push_back(s);
    }

    bool allOk = true;
    for (auto it = buckets.begin(); it != buckets.end(); ++it) {
        if (!m_dal->batchInsert(it.key(), gran, it.value())) {
            qWarning("L2HistoryStore: batchInsert failed for %s", qUtf8Printable(it.key()));
            allOk = false;
        }
    }
    return allOk;
}

size_t L2HistoryStore::pendingCount() const {
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    return m_writeBuffer.size();
}

}  // namespace ens::datahub
