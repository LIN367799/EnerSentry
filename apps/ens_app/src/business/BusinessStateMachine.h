// src/business/BusinessStateMachine.h
// L4 业务层 —— 三层业务态机 Station/Device/Point 配置态/运行态/统计态
// （ENS-LLD-400 §1.2 模块职责边界 + DevGuide §4.3.1）。
//
// 设计：
//   * 三层 FSM（Station/Device/Point），每层独立状态机 + 跨层一致性约束。
//   * 状态机作用：
//       - Config（配置态）：点表/告警规则/控制字加载中
//       - Running（运行态）：正常采集/告警/控制
//       - Stats（统计态）：停采/统计/归档（仅 Station 层可达）
//   * 跨层一致性：Station 可达 Stats 仅当所有下属 Device 均非 Running。
//   * 迁移触发回调（stateChanged）让外部（UI/落库/告警）订阅。
//
// Phase 3 切片 10 (4.3.1) 落地，2026-08-30。

#pragma once

#include <ens/export.hpp>

#include <QObject>
#include <QString>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace ens::business {

/// 三层实体 ID
using StationId  = uint32_t;
using DeviceId   = uint32_t;
using PointId    = uint32_t;

/// 三层实体共用的状态枚举（语义同 HLD §5.4.2）
enum class BusinessState : uint8_t {
    Config  = 0,   ///< 配置加载/初始化中
    Running = 1,   ///< 正常运行
    Stats   = 2,   ///< 停采/统计/归档（仅 Station 可达）
};

class ENS_BUSINESS_API BusinessStateMachine : public QObject {
    Q_OBJECT
public:
    explicit BusinessStateMachine(QObject* parent = nullptr);
    ~BusinessStateMachine() override;

    // ═══ Device 注册/反注册 ═══
    /// 注册 Device 与其所属 Station + Points；初始状态 Config
    void registerDevice(StationId stationId, DeviceId deviceId,
                        const std::vector<PointId>& points);
    /// 反注册；仅在 Device 处于 Config 或 Stats 时允许
    void unregisterDevice(DeviceId deviceId);

    // ═══ 状态迁移 ═══
    /// 升 Running：要求 Device 所有 Point 已在 Config
    bool toRunning(DeviceId deviceId);
    /// 进入 Stats（仅 Station）：要求 Station 下属 Device 全部非 Running
    bool toStats(StationId stationId);
    /// 回 Config（Station/整站复位）
    bool toConfigStation(StationId stationId);
    /// 回 Config（Device 复位）
    bool toConfigDevice(DeviceId deviceId);
    /// 回 Config（Point 单点复位）
    bool toConfigPoint(PointId pointId);

    // ═══ 查询 ═══
    BusinessState stationState(StationId stationId) const;
    BusinessState deviceState(DeviceId deviceId)   const;
    BusinessState pointState(PointId pointId)       const;

    size_t stationCount() const noexcept;
    size_t deviceCount()  const noexcept;
    size_t pointCount()   const noexcept;

signals:
    void stateChanged(StationId stationId, DeviceId deviceId, PointId pointId,
                      uint8_t oldState, uint8_t newState);

private:
    struct DeviceRec {
        StationId stationId = 0;
        BusinessState state  = BusinessState::Config;
        std::unordered_set<PointId> points;
    };

    /// 跨层一致性：升 Station→Stats 时检查所有下属 Device 均非 Running
    bool canStationEnterStats(StationId stationId) const;

    mutable std::recursive_mutex m_mutex;
    std::unordered_map<StationId, BusinessState>             m_stations;
    std::unordered_map<DeviceId, DeviceRec>                  m_devices;
    std::unordered_map<PointId, std::pair<DeviceId, BusinessState>> m_points;
};

}  // namespace ens::business