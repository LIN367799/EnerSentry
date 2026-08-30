// src/sim/point_generator.cpp ── 见 point_generator.h
#include "point_generator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace ens::sim {

namespace {

// 由 point 命名 + 命名约定启发 slave 类别（Phase 1 教学版样本够用）
// 新增 Liquid/Fire/Meter 命名识别（DoD ① "均非全 0" 要求辅机 baseline 也写）
DeviceKind inferKindFromName(const std::vector<SimPoint>& v) noexcept {
    if (v.empty()) return DeviceKind::Bms;
    for (const auto& p : v) {
        if (p.pointName.find("PCS_")   != std::string::npos) return DeviceKind::Pcs;
        if (p.pointName.find("Meter")  != std::string::npos) return DeviceKind::Meter;
        if (p.pointName.find("Liquid") != std::string::npos) return DeviceKind::Liquid;
        if (p.pointName.find("Fire")   != std::string::npos) return DeviceKind::Fire;
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
    // PCS 参考从站(供 Meter 引用有功):取首个 kind==Pcs,无则保持 0 → evolveMeter 取 0
    for (uint8_t id : m_slaves) {
        if (inferKindFromName(m_pt->onSlave(id)) == DeviceKind::Pcs) {
            m_pcsRefSlave = id;
            break;
        }
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
    const uint32_t dtMs = m_cfg.tickMs;

    // 推进所有 FaultSession FSM（RECOVERING 斜率回归 / ACTIVE→RECOVERING 自然到期）
    if (m_fi != nullptr) {
        m_fi->tickSessions(dtMs);
    }

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
        } catch (...) {
            // SIM-IMP §6.2:生成线程异常时该从站值保持上一帧
        }

        // 故障叠加：遍历本从站所有 HoldingRegister 点,调 resolveOverride 覆盖 m_work
        // （B7 范围:仅寄存器值覆盖;告警字置位延后到 B8）
        if (m_fi != nullptr) {
            const auto& v = m_pt->onSlave(slave);
            for (const auto& p : v) {
                if (p.regType != RegisterType::HoldingRegister) continue;
                const FaultEffect eff = m_fi->resolveOverride(slave, p.registerAddr);
                if (!eff.active) continue;
                // dropLink 标志由 B8 IO 层消费,此处不写
                if (eff.dropLink) continue;
                // B8 告警字置位:OverTemp→bit0,CellVoltage(value>0 过压→bit1 / value<0 欠压→bit2)
                if (eff.type == FaultType::OverTemp) {
                    m_work[slave].alarmWord |= (uint16_t(1) << 0);
                } else if (eff.type == FaultType::CellVoltage) {
                    m_work[slave].alarmWord |= (uint16_t(1) << (eff.value > 0.0f ? 1 : 2));
                }
                // 工程值 → raw:raw = (eng - offset) / scale,clamp uint16
                const float scale = (p.scaleFactor != 0.0f) ? p.scaleFactor : 1.0f;
                const float rawF  = (eff.value - p.offset) / scale;
                const uint16_t raw = static_cast<uint16_t>(std::clamp(rawF, 0.0f, 65535.0f));
                m_work[slave].setHolding(p.registerAddr, raw);
            }
        }

        // CoW publish 到 RCU;m_bank==nullptr(纯单测)时 work 即最终结果
        auto snap = std::make_shared<SlaveRegset>(m_work[slave]);
        if (m_bank != nullptr) {
            m_bank->publish(slave, snap);
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
    // Meter 把首个 PCS 的有功当参考;无 PCS 时取 0。所有寄存器类型都写非零 baseline
    // (DoD ① "均非全 0" 约束:即便 InputRegister 也给个 baseline 占位)
    const double refKw = (m_pcsRefSlave != 0) ? m_state[m_pcsRefSlave].pcs_active_kw : 0.0;
    const auto& v = m_pt->onSlave(slave);
    for (const auto& p : v) {
        if (!p.enabled) continue;
        double eng = 1.0;   // baseline 占位
        if (p.pointName.find("ActiveP") != std::string::npos ||
            p.pointName.find("TotalE")  != std::string::npos) {
            eng = refKw;
        }
        uint16_t raw = encodeFloat32AsHolding(static_cast<float>(eng), p.scaleFactor);
        if (p.regType == RegisterType::HoldingRegister) {
            work.setHolding(p.registerAddr, raw);
        } else if (p.regType == RegisterType::InputRegister) {
            work.setInput(p.registerAddr, raw);
        }
    }
}

void PointGenerator::evolveAux(uint8_t slave, double dtS) noexcept {
    (void)dtS;
    auto& st = m_state[slave];
    SlaveRegset& work = m_work[slave];
    // 辅机(Liquid/Fire):所有寄存器类型都写非零 baseline
    // (DoD ① "均非全 0":SupplyTemp 25℃ 起步 / Alarm 无报警 = 0)
    const auto& v = m_pt->onSlave(slave);
    for (const auto& p : v) {
        if (!p.enabled) continue;
        double eng = 25.0;   // 液冷供水默认 25℃;消防 status 1 = 待命
        if (p.pointName.find("Alarm") != std::string::npos) {
            eng = 0.0;
        }
        if (p.regType == RegisterType::Coil) {
            // 线圈:1 = active, 0 = inactive
            work.setCoil(p.registerAddr, eng != 0.0);
        } else if (p.regType == RegisterType::DiscreteInput) {
            // 离散输入:与线圈分属不同向量(FC02 读 discretes)
            work.setDiscrete(p.registerAddr, eng != 0.0);
        } else {
            const uint16_t raw = encodeFloat32AsHolding(
                static_cast<float>(eng), p.scaleFactor);
            if (p.regType == RegisterType::HoldingRegister) {
                work.setHolding(p.registerAddr, raw);
            } else if (p.regType == RegisterType::InputRegister) {
                work.setInput(p.registerAddr, raw);
            }
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
