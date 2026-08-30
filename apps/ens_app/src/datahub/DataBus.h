// src/datahub/DataBus.h
// L3 数据中枢 ── 观察者模式广播总线（DevGuide §4.1.3 / ENS-LLD-200 §6 落地补）。
//
// 设计要点：
//   * 订阅表按 pointId 索引 + 独立通配订阅（订阅所有 pointId）
//   * QReadWriteLock 保护订阅表：广播走读锁（订阅多写少），订阅/退订走写锁
//   * subscribe 返 Subscription handle（自增 uint64_t），unsubscribe 按 handle 精确删除
//   * broadcast 同步回调订阅者 onSample — 订阅者实现需轻量（与 L1SnapshotStore 异步读取互补）
//
// 线程模型：采集线程 broadcast → 同步调用订阅者 onSample；订阅者实现须非阻塞
// 或将重活转移至工作线程。广播内部异常由订阅者 try/catch 自管（DataBus 不吞异常）。
//
// 与 DevGuide §4.1.3 简述的差异（V.x 落地补）：
//   * 加 Subscription handle（自增 id）支持精确退订；DevGuide 仅描述"订阅/退订"语义
//   * 明确 broadcast 同步模型 + 订阅者实现约束（轻量/非阻塞）

#pragma once

#include "Sample.h"

#include <cstdint>

#include <QHash>
#include <QReadWriteLock>
#include <QVector>

namespace ens::datahub {

/// 订阅句柄（0 = 无效；subscribe 返非零自增 id）
using Subscription = uint64_t;

/// @brief 订阅者接口（订阅者实现须轻量：同步回调，非阻塞）
class IDataBusSubscriber {
public:
    virtual ~IDataBusSubscriber() = default;

    /// 单点样本回调（broadcast 时调用）
    /// @note 必须 noexcept-friendly：抛异常将终止 broadcast 流程（数据流中断）
    virtual void onSample(const Sample& s) noexcept = 0;
};

/// @brief 数据总线：观察者模式广播（订阅表 QReadWriteLock 保护）
///
/// 线程模型：broadcast 走"读锁下收集匹配订阅者 → 解锁后同步回调"两步走：
///   * 避免订阅者 onSample 内 unsubscribe 引发读→写锁死锁
///   * 订阅者实现须轻量、非阻塞；onSample 内调 broadcast 仍会因 Qt QReadWriteLock
///     不支持递归读而死锁（业务约束：禁止在 onSample 内嵌套 broadcast）
///   * 订阅者 onSample 抛异常将终止本次广播（订阅者自管 try/catch）
class DataBus {
public:
    DataBus() = default;
    ~DataBus() = default;

    DataBus(const DataBus&) = delete;
    DataBus& operator=(const DataBus&) = delete;
    DataBus(DataBus&&) = delete;
    DataBus& operator=(DataBus&&) = delete;

    /// 订阅指定 pointId（多次订阅同一订阅者同一 pointId 返多个 handle，需各自 unsubscribe）
    /// @return 订阅句柄（>0）；失败返 0
    Subscription subscribe(uint32_t pointId, IDataBusSubscriber* sub);

    /// 订阅所有 pointId（通配：每次 broadcast 都触发）
    /// @return 订阅句柄（>0）；失败返 0
    Subscription subscribeWildcard(IDataBusSubscriber* sub);

    /// 按 handle 退订（点订阅 / 通配订阅统一接口）
    /// @return true 找到并删除；false handle 不存在
    bool unsubscribe(Subscription handle) noexcept;

    /// 广播单点样本（采集线程调用）：读锁遍历订阅表，匹配 pointId 或通配，回调 onSample
    /// @note 同步模型：订阅者 onSample 须轻量；抛异常将终止本次广播
    void broadcast(const Sample& s);

    /// 诊断查询：当前活跃订阅数
    size_t subscriberCount() const noexcept;

    /// 诊断查询：通配订阅数
    size_t wildcardCount() const noexcept;

private:
    /// 订阅条目：pointId（UINT32_MAX 表示通配）+ 订阅者指针
    struct Entry {
        uint32_t            pointId = 0;
        IDataBusSubscriber* sub     = nullptr;
    };

    /// 内部退订辅助（写锁外调用）
    bool unsubscribeImpl_locked(Subscription handle) noexcept;

    mutable QReadWriteLock         m_lock;
    QVector<Entry>                 m_entries;      // 按 pointId 索引 + 通配混杂
    QHash<Subscription, size_t>    m_handleIndex;  // handle → m_entries 索引（O(1) 退订）
    Subscription                   m_nextHandle{1};
};

}  // namespace ens::datahub
