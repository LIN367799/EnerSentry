// src/business/DeviceSboGuard.cpp
// L4 业务层 —— DeviceSboGuard 实现（ENS-LLD-400 §3.2 ADR-23）。

#include "DeviceSboGuard.h"

#include <chrono>

namespace ens::business {

namespace {
/// 单调时钟 ms（steady_clock；独立于 wall clock，不受 NTP/手动调时影响）
inline int64_t monotonicNowMs() noexcept {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace

DeviceSboGuard::DeviceSboGuard(QObject* parent) : QObject(parent) {}

DeviceSboGuard::~DeviceSboGuard() = default;

bool DeviceSboGuard::tryAcquire(const SboDeviceKey& key, const QString& sequenceId,
                                const QString& operatorName, ArmedOccupant* out) {
    QMutexLocker lk(&m_mutex);
    auto it = m_buckets.find(key);
    if (it != m_buckets.end()) {
        // 已占用：发 rejected 信号（busyBy + 已占用时长）
        const int64_t elapsed = monotonicNowMs() - it->armedSinceMonoMs;
        qDebug("DeviceSboGuard::tryAcquire REJECT key=(link=%u,slave=%u,reg=0x%x) busyBy=%s seqReq=%s elapsed=%lld",
               key.linkId, key.slaveId, key.registerAddr,
               qUtf8Printable(it->operatorName), qUtf8Printable(sequenceId),
               (long long)elapsed);
        emit armedRejected(sequenceId, key, it->operatorName, elapsed);
        return false;
    }
    ArmedOccupant occ;
    occ.sequenceId        = sequenceId;
    occ.operatorName      = operatorName;
    occ.armedSinceMonoMs  = monotonicNowMs();
    occ.timeoutMs         = 5000;                       // 默认 5s（急停可外部改）
    m_buckets.insert(key, occ);
    if (out != nullptr) *out = occ;
    lk.unlock();
    emit armedAcquired(sequenceId, key);
    return true;
}

void DeviceSboGuard::release(const SboDeviceKey& key, const QString& sequenceId) {
    QMutexLocker lk(&m_mutex);
    auto it = m_buckets.find(key);
    if (it == m_buckets.end()) return;
    if (it->sequenceId != sequenceId) {
        // 序列不匹配：拒绝释放（防 A 释放 B 的锁）
        return;
    }
    m_buckets.erase(it);
    lk.unlock();
    emit armedReleased(sequenceId, key);
}

std::optional<ArmedOccupant> DeviceSboGuard::query(const SboDeviceKey& key) const {
    QMutexLocker lk(&m_mutex);
    auto it = m_buckets.find(key);
    if (it == m_buckets.end()) return std::nullopt;
    return it.value();
}

QList<SboDeviceKey> DeviceSboGuard::listActiveArmed() const {
    QMutexLocker lk(&m_mutex);
    return m_buckets.keys();
}

size_t DeviceSboGuard::activeCount() const {
    QMutexLocker lk(&m_mutex);
    return static_cast<size_t>(m_buckets.size());
}

}  // namespace ens::business