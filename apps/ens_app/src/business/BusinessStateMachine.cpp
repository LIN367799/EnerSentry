// src/business/BusinessStateMachine.cpp
// L4 业务层 —— BusinessStateMachine 实现。

#include "BusinessStateMachine.h"

#include <algorithm>

namespace ens::business {

BusinessStateMachine::BusinessStateMachine(QObject* parent) : QObject(parent) {}

BusinessStateMachine::~BusinessStateMachine() = default;

void BusinessStateMachine::registerDevice(StationId stationId, DeviceId deviceId,
                                          const std::vector<PointId>& points) {
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    // Station 自动创建（Config 初态）
    if (m_stations.find(stationId) == m_stations.end()) {
        m_stations[stationId] = BusinessState::Config;
    }
    DeviceRec rec;
    rec.stationId = stationId;
    rec.state     = BusinessState::Config;
    for (PointId p : points) {
        rec.points.insert(p);
        m_points[p] = {deviceId, BusinessState::Config};
    }
    m_devices[deviceId] = std::move(rec);
}

void BusinessStateMachine::unregisterDevice(DeviceId deviceId) {
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    auto it = m_devices.find(deviceId);
    if (it == m_devices.end()) return;
    if (it->second.state == BusinessState::Running) return;   // 运行中拒删
    for (PointId p : it->second.points) m_points.erase(p);
    m_devices.erase(it);
}

bool BusinessStateMachine::toRunning(DeviceId deviceId) {
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    auto it = m_devices.find(deviceId);
    if (it == m_devices.end()) return false;
    if (it->second.state == BusinessState::Running) return true;   // 幂等

    const uint8_t oldS = static_cast<uint8_t>(it->second.state);
    it->second.state = BusinessState::Running;
    // 所属 Station 若为 Config → 自动升 Running（Device 全部 Running 是 Station 升 Running 的充分信号）
    auto sit = m_stations.find(it->second.stationId);
    if (sit != m_stations.end() && sit->second == BusinessState::Config) {
        const uint8_t oldSt = static_cast<uint8_t>(sit->second);
        sit->second = BusinessState::Running;
        emit stateChanged(it->second.stationId, deviceId, 0, oldSt,
                          static_cast<uint8_t>(BusinessState::Running));
    }
    emit stateChanged(it->second.stationId, deviceId, 0, oldS,
                      static_cast<uint8_t>(BusinessState::Running));
    return true;
}

bool BusinessStateMachine::toStats(StationId stationId) {
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    auto sit = m_stations.find(stationId);
    if (sit == m_stations.end()) return false;
    if (!canStationEnterStats(stationId)) return false;          // 有下属 Running
    const uint8_t oldS = static_cast<uint8_t>(sit->second);
    sit->second = BusinessState::Stats;
    emit stateChanged(stationId, 0, 0, oldS, static_cast<uint8_t>(BusinessState::Stats));
    return true;
}

bool BusinessStateMachine::toConfigStation(StationId stationId) {
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    auto sit = m_stations.find(stationId);
    if (sit == m_stations.end()) return false;
    const uint8_t oldS = static_cast<uint8_t>(sit->second);
    sit->second = BusinessState::Config;
    emit stateChanged(stationId, 0, 0, oldS, static_cast<uint8_t>(BusinessState::Config));
    return true;
}

bool BusinessStateMachine::toConfigDevice(DeviceId deviceId) {
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    auto it = m_devices.find(deviceId);
    if (it == m_devices.end()) return false;
    // toConfig 是强制复位,任何状态都允许;运行中的设备复位前应先 stop polling(上层职责)
    const uint8_t oldS = static_cast<uint8_t>(it->second.state);
    it->second.state = BusinessState::Config;
    emit stateChanged(it->second.stationId, deviceId, 0, oldS,
                      static_cast<uint8_t>(BusinessState::Config));
    return true;
}

bool BusinessStateMachine::toConfigPoint(PointId pointId) {
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    auto it = m_points.find(pointId);
    if (it == m_points.end()) return false;
    const uint8_t oldS = static_cast<uint8_t>(it->second.second);
    it->second.second = BusinessState::Config;
    emit stateChanged(0, it->second.first, pointId, oldS,
                      static_cast<uint8_t>(BusinessState::Config));
    return true;
}

BusinessState BusinessStateMachine::stationState(StationId stationId) const {
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    auto it = m_stations.find(stationId);
    return (it != m_stations.end()) ? it->second : BusinessState::Config;
}

BusinessState BusinessStateMachine::deviceState(DeviceId deviceId) const {
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    auto it = m_devices.find(deviceId);
    return (it != m_devices.end()) ? it->second.state : BusinessState::Config;
}

BusinessState BusinessStateMachine::pointState(PointId pointId) const {
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    auto it = m_points.find(pointId);
    return (it != m_points.end()) ? it->second.second : BusinessState::Config;
}

size_t BusinessStateMachine::stationCount() const noexcept {
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    return m_stations.size();
}

size_t BusinessStateMachine::deviceCount() const noexcept {
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    return m_devices.size();
}

size_t BusinessStateMachine::pointCount() const noexcept {
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    return m_points.size();
}

bool BusinessStateMachine::canStationEnterStats(StationId stationId) const {
    for (const auto& [devId, rec] : m_devices) {
        if (rec.stationId == stationId && rec.state == BusinessState::Running) {
            return false;
        }
    }
    return true;
}

}  // namespace ens::business