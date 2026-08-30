// src/business/DeviceSboGuard.h
// L4 业务层 —— 设备级 SBO 锁（ENS-LLD-400 §3.2）。
//
// 设计：
//   * 二维 Key (linkId, slaveId, registerAddr) 分桶互斥；同设备同寄存器同一时刻
//     仅允许 1 个 Armed（不同设备/寄存器可并发）。
//   * ArmedOccupant 纯数据（绝不持 QTimer 指针，避免跨线程悬空）；
//     倒计时由 SboStateMachine 独立管理（§3.4 链路抖动/独占倒计时）。
//   * 跨线程安全：m_buckets 由 QMutex 保护，可被任意线程 query/release。

#pragma once

#include <ens/export.hpp>

#include <QObject>
#include <QHash>
#include <QHashFunctions>
#include <QMutex>
#include <QString>
#include <QList>
#include <cstdint>
#include <optional>

namespace ens::business {

/// 设备级 SBO 锁 Key（二维：(链路+从站) + 寄存器地址）
struct SboDeviceKey {
    uint32_t linkId       = 0;   ///< 通信链路 ID
    uint32_t slaveId      = 0;   ///< Modbus 从站号
    uint32_t registerAddr = 0;   ///< 操作寄存器地址

    bool operator==(const SboDeviceKey& o) const {
        return linkId == o.linkId && slaveId == o.slaveId && registerAddr == o.registerAddr;
    }
};

}  // namespace ens::business

// Qt5/6：手写 FNV-1a hash（qHashMulti 在 Qt 5.15.2 <QHashFunctions> 中存在但
// MSVC 在 SHARED 导出模板实例化时常报"找不到标识符",改用最简 FNV-1a 更稳）
inline uint qHash(const ens::business::SboDeviceKey& k, uint seed = 0) {
    uint h = seed;
    h = (h ^ k.linkId)       * 16777619u;
    h = (h ^ k.slaveId)      * 16777619u;
    h = (h ^ k.registerAddr) * 16777619u;
    return h;
}

namespace ens::business {

/// 当前 Armed 占用信息（纯数据，绝不含 QObject 指针）
struct ArmedOccupant {
    QString sequenceId;                ///< SBO 序列 ID
    QString operatorName;              ///< 操作员
    int64_t armedSinceMonoMs = 0;      ///< 进入 Armed 的单调时刻（steady_clock）
    int64_t timeoutMs        = 5000;   ///< 超时阈值（常规 5000 / 急停 3000）
};

/// 设备级 SBO 逻辑锁守卫（分桶互斥，ADR-23）
class ENS_BUSINESS_API DeviceSboGuard : public QObject {
    Q_OBJECT
public:
    explicit DeviceSboGuard(QObject* parent = nullptr);
    ~DeviceSboGuard() override;

    /// 尝试获取设备级锁；false=该设备该寄存器已有 Armed（并发冲突）
    /// @param key 设备 + 寄存器
    /// @param sequenceId SBO 序列 ID（释放时校验防误释放）
    /// @param operatorName 操作员
    /// @param out [out] 占用信息（成功时填充）
    /// @return true 锁空闲并已占用
    bool tryAcquire(const SboDeviceKey& key, const QString& sequenceId,
                    const QString& operatorName, ArmedOccupant* out = nullptr);

    /// 释放锁（Operate/Cancel/Aborted 任一终止时调用）
    /// @note sequenceId 防误释放：若不匹配则拒释放（拒绝外部意外抢占）
    void release(const SboDeviceKey& key, const QString& sequenceId);

    /// 查询当前占用（返回只读元数据，不含任何 QObject 指针）
    std::optional<ArmedOccupant> query(const SboDeviceKey& key) const;

    /// 当前所有活跃 Armed 的 Key 列表（断线扫描 / 启动恢复用）
    QList<SboDeviceKey> listActiveArmed() const;

    /// 当前活跃 Armed 数量（诊断）
    size_t activeCount() const;

signals:
    void armedAcquired(const QString& sequenceId, const SboDeviceKey& key);
    void armedRejected(const QString& sequenceId, const SboDeviceKey& key,
                       const QString& busyBy, int64_t elapsedMs);
    void armedReleased(const QString& sequenceId, const SboDeviceKey& key);

private:
    QHash<SboDeviceKey, ArmedOccupant> m_buckets;  ///< 设备 → Armed 占用
    mutable QMutex m_mutex;                          ///< 保护 m_buckets
};

}  // namespace ens::business