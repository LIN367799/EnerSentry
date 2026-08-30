// src/datahub/L2HistoryStore.h
// L3 数据中枢 ── L2 历史持久化（双缓冲 Swap + 背压 + 按月分桶批量 INSERT）（ENS-LLD-200 §4.5）。
//
// 关键设计：
//   * 接收 DownSampledSample(由 DownSampler::rollUp 产出),按月分桶批量落库
//   * 双缓冲:enqueue 锁下 push_back + 背压;flush 时 swap(锁持有 < 0.1μs)
//   * 背压:超过 capacity 丢最老 + qWarning(LLD §4.5 backpressure)
//   * 同步 flush() 供测试用;生产模式由上层 QTimer 周期性调 flush()
//   * 异步 QMetaObject::invokeMethod 触发(LLD §4.5 描述):本切片简化,V.x 修订

#pragma once

#include "DownSampler.h"
#include "SQLiteDataAccess.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

#include <QObject>

namespace ens::datahub {

class L2HistoryStore : public QObject {
    Q_OBJECT
public:
    static constexpr size_t kDefaultCapacity = 100000;   // LLD §4.5 默认 100K(20s @5000 pts/s)

    /// @param dal 非 own,生命周期由调用方管理
    explicit L2HistoryStore(SQLiteDataAccess* dal, QObject* parent = nullptr);
    ~L2HistoryStore() override;

    L2HistoryStore(const L2HistoryStore&) = delete;
    L2HistoryStore& operator=(const L2HistoryStore&) = delete;
    L2HistoryStore(L2HistoryStore&&) = delete;
    L2HistoryStore& operator=(L2HistoryStore&&) = delete;

    /// 入队一条降采样结果(可能触发背压丢弃)
    void enqueueSample(const DownSampledSample& s);

    /// 同步 flush:swap buffer → 按月分桶 → 每桶事务内 batchInsert
    /// @return true 全部月库写入成功;false 任一失败(已回滚的月库部分写入丢失)
    bool flush();

    /// 容量配置(背压阈值,默认 100K)
    void setCapacity(size_t c) noexcept { m_capacity = c; }
    size_t capacity() const noexcept { return m_capacity; }

    /// 诊断:当前 buffer 中待 flush 数量
    size_t pendingCount() const;

    /// 诊断:累计丢弃数(背压触发)
    size_t droppedCount() const noexcept { return m_droppedCount.load(); }

private:
    SQLiteDataAccess* m_dal;                              // 弱引用,非 own
    mutable std::mutex m_bufferMutex;
    std::vector<DownSampledSample> m_writeBuffer;
    size_t m_capacity = kDefaultCapacity;
    std::atomic<size_t> m_droppedCount{0};
};

}  // namespace ens::datahub
