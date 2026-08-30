// src/sim/point_generator.h
// 测试台物理演化器 ── Phase 1 Track B B5（ENS-LLD-SIM §2.2.1 / §4.5.2, ENS-SIM-IMP §4）。
//
// 职责：
//   - 持有 std::mt19937(SimConfig::seed) — NFR-TEST-01 确定性
//   - evolveBms/Pcs/Meter/Aux 按 SIM-IMP §4 物理常数推一 tick 演化
//   - generateTick(Builder&):编排全部从站迭代 + 调用 FaultInjector.resolveOverride 叠加 +
//     publish 到 RegisterBank(RCU 替换 shared_ptr<const SlaveRegset>)
//
// 关键约束：
//   - **确定性** → 同 seed 两次进化序列 bit-for-bit 一致(UT-SIM-02 DoD)
//   - **数据契约严格对齐** → 写入 raw = engineering / scaleFactor (Phase 2 3.1.5 round-trip)
//   - **OCV/LFP 边界** SOC=0/100 钳位(IEEE 754 to_double 安全输入)
//   - **NaN/Inf 防御** evolveBms 等 try-catch,该从站保持上一帧(SIM-IMP §6.2)

#pragma once

#include <cstdint>

#include <memory>
#include <random>
#include <string>
#include <vector>

#include "core/point_table.h"
#include "sim/register_bank.h"
#include "sim/FaultInjector.h"
#include "sim_config.h"

namespace ens::sim {

// ─────────────────────────────────────────────────────────────────────────────
// 默认物理常量（SIM-IMP §4.2 / §4.3）—— 可由外部 JSON 覆盖,Phase 1 B5 用默认
// ─────────────────────────────────────────────────────────────────────────────
struct PhysicsConstants {
    // OCV(SOC) → V,线性插值。Phase 1 B5 用 SIM-IMP §4.1 LFP 11 点表(SOC% 0/5/10/20/30/40/50/60/70/80/90/100)
    static constexpr double kOcvTableSocPct[12] = { 0, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
    static constexpr double kOcvTableVolts[12]  = { 2.50, 2.80, 3.00, 3.20, 3.26, 3.28, 3.30, 3.32, 3.35, 3.40, 3.45, 3.65 };

    // 热模型：dT = (k_heat * I² * R_int − k_cool * (T − T_amb)) * dt
    double k_heat = 0.0008;       // ℃·s/(A²·Ω)
    double R_int  = 0.005;        // Ω 整簇等效
    double k_cool = 0.02;         // 1/s
    double T_amb  = 25.0;         // ℃

    // 默认初值（SIM-IMP §4.3）
    double soc0_pct      = 80.0;
    double soh0_pct      = 99.0;
    double maxTemp0_c    = 35.0;
    double avgTemp0_c    = 33.0;
    double pcsActiveKw0  = 0.0;
    double pcsReactiveKvar0 = 0.0;
    double pcsLineV0     = 400.0;
    double pcsFreqHz0    = 50.0;

    // 簇参数
    double capacity_ah  = 280.0;
    double cells_series = 112.0;   // V_total = OCV * cells_series

    // PCS/Meter 行为：可在 evolve 前 overlay（默认 0 → 测试台启动即合理）
    double pcsActiveKwSetpoint   = 0.0;       // 期望有功（>0 放电, <0 充电）
    double pcsReactiveKvarSetpoint = 0.0;
    double bmsCurrentA_setpoint  = 0.0;       // 期望流经簇的电流
};

struct SlaveRuntimeState {
    double soc_pct      = 80.0;     // SOC %
    double soh_pct      = 99.0;     // SOH %
    double max_temp_c   = 35.0;     // 簇最高温 ℃
    double avg_temp_c   = 33.0;     // 簇平均温 ℃
    double total_v      = 0.0;      // 簇总压 V
    double current_a    = 0.0;      // 簇电流 A
    double pcs_active_kw = 0.0;
    double pcs_reactive_kvar = 0.0;
    double pcs_line_v   = 400.0;
    double pcs_freq_hz  = 50.0;
    double aux_status   = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// PointGenerator ── 物理演化器
// ─────────────────────────────────────────────────────────────────────────────
class PointGenerator {
public:
    PointGenerator(SimConfig cfg,
                   std::shared_ptr<const SimPointTable> pt,
                   PhysicsConstants physics = {});

    PointGenerator(const PointGenerator&) = delete;
    PointGenerator& operator=(const PointGenerator&) = delete;

    /// 单 tick 演化（100ms 默认 dt）。返回 void;执行多从站顺序遍历 + RegisterBank.publish。
    /// 不感知 IO 线程;后者只 snapshot 读。
    void generateTick();

    /// 给定 slave 的 RT 内部状态(测试/诊断)
    const SlaveRuntimeState& stateOf(uint8_t slave) const;

    /// 给定 slave 当前 tick 工作副本持有寄存器(测试用,Phase 1 无 SimulatorEngine 时)
    const SlaveRegset& workOf(uint8_t slave) const;

    /// 给 RCU bank 注入（SimulatorEngine 调用,Phase 1 暂时未用）
    void attach(RegisterBank* bank) noexcept { m_bank = bank; }

    /// 注入 FaultInjector（B7 Phase 3 切片 11）；不拥有,SimulatorEngine/外部持有生命周期
    void attachFi(FaultInjector* fi) noexcept { m_fi = fi; }

    /// 强制重置 seed（场景脚本重启时）—— 主程序侧启动一次即用,不应频繁调用
    void reseed(uint32_t s) noexcept;

    /// raw → engineering（与主程序 PointTable decodeToEngineering 等价,简版）
    /// NFR-TEST-01 平台一致性要求:sim 端 encode 与主程序端 decode 必须 round-trip 一致
    static uint16_t encodeFloat32AsHolding(float eng, float scale) noexcept;

    /// engineering → raw(由 generator 写入 SlaveRegset 用)
    static float   decodeFloat32FromHolding(uint16_t raw, float scale) noexcept;

    std::vector<uint8_t> allSlaveIds() const noexcept;

    /// OCV(SOC% clamp + linear interp, SIM-IMP 4.1 LFP 11-pt table) -> voltage V
    /// public for tests and upper FaultInjector reuse
    static double ocvFromSoc(double socPct) noexcept;

private:
    // ── 内部 API ──
    void evolveBms(uint8_t slave, double dtS) noexcept;
    void evolvePcs(uint8_t slave, double dtS) noexcept;
    void evolveMeter(uint8_t slave, double dtS) noexcept;
    void evolveAux(uint8_t slave, double dtS) noexcept;

    /// Cluster total voltage (V) - Phase 1 simplified to OCV (cells_series to come Phase 3.x)
    static double totalVoltageFromOcv(double ocvV) noexcept;

    SimConfig                                 m_cfg;
    std::shared_ptr<const SimPointTable>      m_pt;
    PhysicsConstants                          m_phy;
    std::mt19937                              m_rng;
    std::unordered_map<uint8_t, SlaveRuntimeState> m_state;
    std::unordered_map<uint8_t, SlaveRegset>      m_work;     // 本 tick 工作副本
    std::vector<uint8_t>                           m_slaves;
    uint8_t     m_pcsRefSlave = 0;   // 首个 kind==Pcs 的 slave(Meter 有功参考源),0=无 PCS
    RegisterBank*                                  m_bank = nullptr;   // 非拥有,SimulatorEngine 持有
    FaultInjector*                                 m_fi   = nullptr;   // 非拥有,B7 切片 11 接入
};

}  // namespace ens::sim
