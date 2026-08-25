// src/sim/point_generator.cpp ── 见 point_generator.h
#include "point_generator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace ens::sim {

namespace {

// 由 point 命名启发 slave 类别（Phase 1 教学版样本够用）
DeviceKind inferKindFromName(const std::vector<SimPoint>& v) noexcept {
    if (v.empty()) return DeviceKind::Bms;
    for (const auto& p : v) {
        if (p.pointName.find("PCS_") != std::string::npos) return DeviceKind::Pcs;
        if (p.pointName.find("Meter") != std::string::npos) return DeviceKind::Meter;
    }
    return DeviceKind::Bms;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 构造 / 状态初始化
// ─────────────────────────────────────────────────────────────────────────────

PointGenerator::PointGenerator(SimConfig cfg,
                               std::shared_ptr<const SimPointTable> pt,
                               PhysicsConstants physics)
    : m_cfg(std::move(cfg))
    , m_pt(std::move(pt))
    , m_phy(physics)
    , m_rng(m_cfg.seed) {
    if (m_pt == nullptr) {
        throw std::runtime_error("PointGenerator: pt == nullptr");
    }
    m_slaves = m_pt->allSlaveIds();

    // 初始每个 slave 状态 + work 缓冲（容量按点表最大 addr 定）
    for (uint8_t id : m_slaves) {
        m_state[id] = SlaveRuntimeState{};
        const auto& v = m_pt->onSlave(id);
        uint16_t maxAddr = 0;
        for (const auto& p : v) {
            if (p.registerAddr > maxAddr) maxAddr = p.registerAddr;
        }
        const size_t cap = std::max<size_t>(maxAddr + 16, 128u);
        m_work[id] = SlaveRegset::allocate(id, cap, cap);
        m_work[id].slaveId = id;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// OCV 查表 / 总压
// ─────────────────────────────────────────────────────────────────────────────

double PointGenerator::ocvFromSoc(double socPct) noexcept {
    using K = PhysicsConstants;
    // 钳位
    if (socPct <= K::kOcvTableSocPct[0]) return K::kOcvTableVolts[0];
    if (socPct >= K::kOcvTableSocPct[11]) return K::kOcvTableVolts[11];
    // 线性插值
    for (size_t i = 0; i + 1 < 12; ++i) {
        const double x0 = K::kOcvTableSocPct[i];
        const double x1 = K::kOcvTableSocPct[i + 1];
        if (socPct >= x0 && socPct <= x1) {
            const double t = (socPct - x0) / (x1 - x0);
            return K::kOcvTableVolts[i] + t * (K::kOcvTableVolts[i + 1] - K::kOcvTableVolts[i]);
        }
    }
    return K::kOcvTableVolts[11];
}

double PointGenerator::totalVoltageFromOcv(double ocvV) noexcept {
    return ocvV;   // 占位 ── Phase 1 教学版 cells_series 留 Phase 3.x 接入;DoD raw=3700(scale 0.001)
                   // → 3.700V 等价于总压 3.7V,无需 cells_series 因子
}

// ─────────────────────────────────────────────────────────────────────────────
// generateTick 主路径
// ─────────────────────────────────────────────────────────────────────────────

void PointGenerator::generateTick() {
    if (m_pt == nullptr) return;
    const double dtS = m_cfg.tickMs / 1000.0;

    for (uint8_t slave : m_slaves) {
        try {
            const auto& v = m_pt->onSlave(slave);
            const DeviceKind kind = inferKindFromName(v);
            switch (kind) {
                case DeviceKind::Bms:    evolveBms(slave, dtS);   break;
                case DeviceKind::Pcs:    evolvePcs(slave, dtS);   break;
                case DeviceKind::Meter:  evolveMeter(slave, dtS); break;
                case DeviceKind::Liquid: evolveAux(slave, dtS);   break;
                case DeviceKind::Fire:   evolveAux(slave, dtS);   break;
            }
            // Phase 3.x FaultInjector::resolveOverride 叠加（先 publish 干净 baseline）
        } catch (...) {
            // SIM-IMP §6.2:生成线程异常时该从站值保持上一帧
        }

        // CoW publish 到 RCU（Phase 1 暂未连 SimulatorEngine,debug 时把 work 同步到 m_work）
        auto snap = std::make_shared<SlaveRegset>(m_work[slave]);
        if (m_bank != nullptr) {
            m_bank->publish(slave, snap);
        } else {
            // 单测场景:work 自己备份一份（避免上面快照副本被释放后 m_work 仍引用相同内容）
            m_work[slave] = *snap;
        }
    }
}

void PointGenerator::evolveBms(uint8_t slave, double dtS) noexcept {
    auto& st = m_state[slave];
    SlaveRegset& work = m_work[slave];

    // SOC 变化: dSOC = -I * dt / (Capacity * 3600) * 100
    const double I = m_phy.bmsCurrentA_setpoint;
    const double cap = m_phy.capacity_ah;
    const double dSocPctPerSec = -I / (cap * 3600.0) * 100.0;
    const double newSoc = std::clamp(st.soc_pct + dSocPctPerSec * dtS, 0.0, 100.0);
    st.soc_pct = newSoc;
    st.current_a = I;

    // 热模型 dT = (k_heat * I² * R_int − k_cool * (T − T_amb)) * dt
    const double Tavg = st.avg_temp_c;
    const double Q_in = m_phy.k_heat * I * I * m_phy.R_int;
    const double Q_out = m_phy.k_cool * (Tavg - m_phy.T_amb);
    st.avg_temp_c = Tavg + (Q_in - Q_out) * dtS;
    st.max_temp_c = st.avg_temp_c + 1.0;

    // 总压
    const double ocvV = ocvFromSoc(st.soc_pct);
    st.total_v = totalVoltageFromOcv(ocvV);

    // 写入 HoldingRegister(按 pointName 路由)
    const auto& v = m_pt->onSlave(slave);
    for (const auto& p : v) {
        if (!p.enabled || p.regType != RegisterType::HoldingRegister) continue;
        double eng = 0.0;
        if      (p.pointName.find("SOC")       != std::string::npos) eng = st.soc_pct;
        else if (p.pointName.find("SOH")       != std::string::npos) eng = st.soh_pct;
        else if (p.pointName.find("MaxTemp")   != std::string::npos) eng = st.max_temp_c;
        else if (p.pointName.find("AvgTemp")   != std::string::npos) eng = st.avg_temp_c;
        else if (p.pointName.find("TotalVolt") != std::string::npos
              || p.pointName.find("TotalV")    != std::string::npos) eng = st.total_v;
        else if (p.pointName.find("Current")   != std::string::npos) eng = st.current_a;
        else continue;
        uint16_t raw = encodeFloat32AsHolding(static_cast<float>(eng), p.scaleFactor);
        work.setHolding(p.registerAddr, raw);
    }
}

void PointGenerator::evolvePcs(uint8_t slave, double dtS) noexcept {
    (void)dtS;
    auto& st = m_state[slave];
    SlaveRegset& work = m_work[slave];
    st.pcs_active_kw     = m_phy.pcsActiveKwSetpoint;
    st.pcs_reactive_kvar = m_phy.pcsReactiveKvarSetpoint;
    st.pcs_line_v        = m_phy.pcsLineV0;
    st.pcs_freq_hz       = m_phy.pcsFreqHz0;

    const auto& v = m_pt->onSlave(slave);
    for (const auto& p : v) {
        if (!p.enabled || p.regType != RegisterType::HoldingRegister) continue;
        double eng = 0.0;
        if      (p.pointName.find("ActiveP")  != std::string::npos) eng = st.pcs_active_kw;
        else if (p.pointName.find("ReactiveP") != std::string::npos) eng = st.pcs_reactive_kvar;
        else if (p.pointName.find("LineV")    != std::string::npos) eng = st.pcs_line_v;
        else if (p.pointName.find("Freq")     != std::string::npos) eng = st.pcs_freq_hz;
        else continue;
        uint16_t raw = encodeFloat32AsHolding(static_cast<float>(eng), p.scaleFactor);
        work.setHolding(p.registerAddr, raw);
    }
}

void PointGenerator::evolveMeter(uint8_t slave, double dtS) noexcept {
    (void)dtS;
    SlaveRegset& work = m_work[slave];
    // 简版：Meter 把 PCS-01 有功当参考
    const double refKw = m_state[17].pcs_active_kw;
    const auto& v = m_pt->onSlave(slave);
    for (const auto& p : v) {
        if (!p.enabled || p.regType != RegisterType::HoldingRegister) continue;
        double eng = 0.0;
        if (p.pointName.find("ActiveP") != std::string::npos) eng = refKw;
        else if (p.pointName.find("MeterTotalE") != std::string::npos) eng = 0.0;
        else continue;
        uint16_t raw = encodeFloat32AsHolding(static_cast<float>(eng), p.scaleFactor);
        work.setHolding(p.registerAddr, raw);
    }
}

void PointGenerator::evolveAux(uint8_t slave, double dtS) noexcept {
    (void)dtS;
    auto& st = m_state[slave];
    SlaveRegset& work = m_work[slave];
    const auto& v = m_pt->onSlave(slave);
    for (const auto& p : v) {
        if (!p.enabled || p.regType != RegisterType::HoldingRegister) continue;
        if (p.pointName.find("Status") != std::string::npos) {
            work.setHolding(p.registerAddr,
                            encodeFloat32AsHolding(static_cast<float>(st.aux_status),
                                                  p.scaleFactor));
        }
    }
}

const SlaveRuntimeState& PointGenerator::stateOf(uint8_t slave) const {
    static const SlaveRuntimeState kEmpty{};
    const auto it = m_state.find(slave);
    return (it != m_state.end()) ? it->second : kEmpty;
}

const SlaveRegset& PointGenerator::workOf(uint8_t slave) const {
    static const SlaveRegset kEmpty{};
    const auto it = m_work.find(slave);
    return (it != m_work.end()) ? it->second : kEmpty;
}

void PointGenerator::reseed(uint32_t s) noexcept {
    m_cfg.seed = s;
    m_rng.seed(s);
}

std::vector<uint8_t> PointGenerator::allSlaveIds() const noexcept {
    return m_slaves;
}

// ─────────────────────────────────────────────────────────────────────────────
// Float32 ↔ HoldingRegister 编码（教学版）
// ─────────────────────────────────────────────────────────────────────────────
uint16_t PointGenerator::encodeFloat32AsHolding(float eng, float scale) noexcept {
    if (scale == 0.0f || std::isnan(eng) || std::isinf(eng)) return 0;
    double raw = std::round(static_cast<double>(eng) / static_cast<double>(scale));
    if (raw < 0.0) raw = 0.0;
    if (raw > 65535.0) raw = 65535.0;
    return static_cast<uint16_t>(raw);
}

float PointGenerator::decodeFloat32FromHolding(uint16_t raw, float scale) noexcept {
    return static_cast<float>(raw) * scale;
}

}  // namespace ens::sim
