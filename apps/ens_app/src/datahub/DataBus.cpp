// src/datahub/DataBus.cpp
// L3 数据中枢 ── 观察者模式广播总线实现（DevGuide §4.1.3 / ENS-LLD-200 §6 落地补）。

#include "DataBus.h"

#include <cstdint>

#include <QReadLocker>
#include <QWriteLocker>

namespace ens::datahub {

constexpr uint32_t kWildcardPointId = UINT32_MAX;   // 通配订阅哨兵

Subscription DataBus::subscribe(uint32_t pointId, IDataBusSubscriber* sub) {
    if (sub == nullptr) return 0;
    QWriteLocker lock(&m_lock);
    const Subscription h = m_nextHandle++;
    const size_t idx = m_entries.size();
    m_entries.append(Entry{pointId, sub});
    m_handleIndex.insert(h, idx);
    return h;
}

Subscription DataBus::subscribeWildcard(IDataBusSubscriber* sub) {
    return subscribe(kWildcardPointId, sub);
}

bool DataBus::unsubscribeImpl_locked(Subscription handle) noexcept {
    auto it = m_handleIndex.find(handle);
    if (it == m_handleIndex.end()) return false;

    // 退订策略：与末尾交换 + pop_back（O(1)），同时修复被换位的 handle 索引
    const size_t idx = it.value();
    const size_t last = m_entries.size() - 1;
    if (idx != last) {
        // 找 last 位置 entry 的 handle（m_handleIndex 反查 entry.handle）
        // 简化：遍历 m_handleIndex 找 last 索引的 handle（O(N) 仅在 unsubscribe 时）
        for (auto hit = m_handleIndex.begin(); hit != m_handleIndex.end(); ++hit) {
            if (hit.value() == last) {
                hit.value() = idx;
                break;
            }
        }
        m_entries[idx] = m_entries[last];
    }
    m_entries.removeLast();
    m_handleIndex.erase(it);
    return true;
}

bool DataBus::unsubscribe(Subscription handle) noexcept {
    if (handle == 0) return false;
    QWriteLocker lock(&m_lock);
    return unsubscribeImpl_locked(handle);
}

void DataBus::broadcast(const Sample& s) {
    // 两步走：读锁下收集要回调的订阅者指针,解锁后逐个调 onSample
    // 避免订阅者 onSample 内 unsubscribe(broadcast 持读锁 → 写锁死锁)
    // 与 Qt QReadWriteLock "同一线程不可重入" 语义一致
    QVector<IDataBusSubscriber*> targets;
    {
        QReadLocker lock(&m_lock);
        targets.reserve(m_entries.size());
        for (const Entry& e : m_entries) {
            if (e.pointId == s.pointId || e.pointId == kWildcardPointId) {
                if (e.sub) targets.append(e.sub);
            }
        }
    }
    // 解锁后回调:订阅者实现须轻量、非阻塞;抛异常将终止本次广播(订阅者自管)
    for (IDataBusSubscriber* sub : targets) {
        sub->onSample(s);
    }
}

size_t DataBus::subscriberCount() const noexcept {
    QReadLocker lock(&m_lock);
    return static_cast<size_t>(m_entries.size());
}

size_t DataBus::wildcardCount() const noexcept {
    QReadLocker lock(&m_lock);
    size_t n = 0;
    for (const auto& e : m_entries) {
        if (e.pointId == kWildcardPointId) ++n;
    }
    return n;
}

}  // namespace ens::datahub
