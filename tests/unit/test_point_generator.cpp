// tests/unit/test_point_generator.cpp
// Tier 2 UT-SIM 单测 —─ 切片 4 DoD：
//   UT-SIM-01  写入 raw=3700 (scale 0.001) → 工程值 3.700V
//   UT-SIM-02  同 seed 两次生成序列一致 (NFR-TEST-01 确定性)
//   *辅助*     放电电流>0 跑 100 tick → SOC 单调降, 温度 I²R 升
//   *辅助*     OCV 边界: SOC=0/100 钳位不报错不抛出
//
// 全部走单线程同步 + sim::PointGenerator + 不依赖 Qt/Socket/串口

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using Catch::Approx;

#include <cstdint>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/point_table.h"
#include "sim/point_generator.h"
#include "sim/sim_config.h"

using namespace ens::sim;

namespace {

// 测试夹具：pointtable sample.json 用 CMake file(COPY) 部署到 build root test_data
std::shared_ptr<SimPointTable> makeLoadedTable() {
    static const std::filesystem::path candidates[] = {
        std::filesystem::path{L"test_data/sim_pointtable_sample.json"},
        std::filesystem::path{L"../test_data/sim_pointtable_sample.json"},
        std::filesystem::path{L"../../bin/Debug/test_data/sim_pointtable_sample.json"},
    };
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            return SimPointTable::loadFromJsonFile(p);
        }
    }
    throw std::runtime_error("PointGenerator test: sample JSON not found in any candidate");
}

}  // namespace

TEST_CASE("UT-SIM-01 raw=3700 scale=0.001 round-trip yields 3.700V engineering",
          "[master][sim][point_generator][DoD][UT-SIM-01]") {
    // DoD 第 1 条: 数据契约严格对齐 — sim 端 encode 与主程序端 decode 必须 round-trip 一致。
    // sim encodeFloat32AsHolding(3.700f, 0.001f) → raw 应为 3700 (round-trip)
    REQUIRE(PointGenerator::encodeFloat32AsHolding(3.700f, 0.001f) == 3700u);

    // decode 反向也得对(scale 0.001 → 3.700V)
    REQUIRE(PointGenerator::decodeFloat32FromHolding(3700, 0.001f)
            == Catch::Approx(3.700f).margin(1e-3f));

    // 边界: 负数 → 0
    REQUIRE(PointGenerator::encodeFloat32AsHolding(-1.0f, 0.001f) == 0u);
    // 边界: 超 16-bit → 65535 钳位
    REQUIRE(PointGenerator::encodeFloat32AsHolding(100.0f, 0.001f) == 65535u);
    // 边界: NaN/Inf → 0
    REQUIRE(PointGenerator::encodeFloat32AsHolding(std::nanf(""), 0.001f) == 0u);
    REQUIRE(PointGenerator::encodeFloat32AsHolding(std::numeric_limits<float>::infinity(),
                                                    0.001f) == 0u);
    // 边界: scale=0 → 0（防御除零）
    REQUIRE(PointGenerator::encodeFloat32AsHolding(3.7f, 0.0f) == 0u);
}

TEST_CASE("UT-SIM-02 same seed produces bit-identical 100 tick sequence (NFR-TEST-01)",
          "[master][sim][point_generator][DoD][UT-SIM-02][NFR-TEST-01]") {
    auto pt = makeLoadedTable();

    auto runOnce = [&]() {
        SimConfig cfg;
        cfg.tickMs = 100;
        cfg.seed   = 42;             // 同 seed(DoD 第 2 条)
        PointGenerator g(cfg, pt);
        for (int t = 0; t < 100; ++t) g.generateTick();
        // 抓 slave=1(Rack-01_BMS)的 8 个关键寄存器 raw 值作为 fingerprint
        const auto& work = g.workOf(1);
        std::vector<uint16_t> fp;
        for (uint16_t addr = 0x1000; addr <= 0x100F; ++addr) {
            fp.push_back(work.getHolding(addr));
        }
        return fp;
    };
    const auto fp1 = runOnce();
    const auto fp2 = runOnce();
    REQUIRE(fp1.size() == fp2.size());
    REQUIRE(fp1 == fp2);   // bit-for-bit 一致
}

TEST_CASE("point_generator: discharge current>0 over 100 ticks monotonically decreases SOC",
          "[master][sim][point_generator][physics]") {
    auto pt = makeLoadedTable();
    SimConfig cfg; cfg.tickMs = 100; cfg.seed = 42;
    PhysicsConstants phy;
    phy.bmsCurrentA_setpoint = 100.0;       // 100A 放电(I>0)
    PointGenerator g(cfg, pt, phy);

    const uint8_t slave = 1;
    std::vector<double> socs;
    socs.push_back(g.stateOf(slave).soc_pct);     // t=0 baseline 80
    for (int t = 10; t <= 100; t += 10) {
        for (int k = 0; k < 10; ++k) g.generateTick();   // 累计 10 ticks
        socs.push_back(g.stateOf(slave).soc_pct);
    }

    // 初始 80%,100 ticks 后应严格 < 80%
    REQUIRE(g.stateOf(slave).soc_pct < 80.0);
    // SOC 必须单调非升(初始 80,之后每个采样 ≤ 前一个)
    for (size_t i = 1; i < socs.size(); ++i) {
        REQUIRE(socs[i] <= socs[i - 1]);
    }
    // 降幅合理范围:100 ticks × 0.1s × -0.00992 = -0.0992%,实际下降 ~0.1%
    REQUIRE((socs.front() - socs.back()) > 0.05);   // 严格单调降 >0.05pct
    REQUIRE((socs.front() - socs.back()) < 1.0);    // 不超过 1pct 上限
}

TEST_CASE("point_generator: OCV clamp SOC=0/100 yields no throw no NaN",
          "[master][sim][point_generator][physics][boundary]") {
    // SIM-IMP §4.1: <0 或 >100 钳位,不抛异常
    REQUIRE(PointGenerator::ocvFromSoc(-50.0) == Approx(2.50));
    REQUIRE(PointGenerator::ocvFromSoc(0.0)   == Approx(2.50));
    // 50% 严格位于表里,用作锚点校验
    const double v50 = PointGenerator::ocvFromSoc(50.0);
    REQUIRE(std::abs(v50 - 3.30) < 0.02);   // 应在 50% 附近,线性插值得 3.30
    REQUIRE(PointGenerator::ocvFromSoc(100.0) == Approx(3.65));
    REQUIRE(PointGenerator::ocvFromSoc(150.0) == Approx(3.65));   // >100 钳到 3.65
}

TEST_CASE("point_generator: temperature rises with I^2 R",
          "[master][sim][point_generator][physics][thermal]") {
    auto pt = makeLoadedTable();
    SimConfig cfg; cfg.tickMs = 100; cfg.seed = 42;
    PhysicsConstants phy;
    phy.bmsCurrentA_setpoint = 200.0;
    phy.R_int = 0.050;       // 50mΩ(大电流下热效应更显著,避开初始平衡)
    PointGenerator g(cfg, pt, phy);

    const uint8_t slave = 1;
    const double t0 = g.stateOf(slave).avg_temp_c;
    for (int i = 0; i < 50; ++i) g.generateTick();   // 50 ticks
    const double t1 = g.stateOf(slave).avg_temp_c;

    REQUIRE(t1 > t0);     // 温度随 I²R 上升
    REQUIRE(std::isfinite(t1));
}

TEST_CASE("point_generator: load 43 points across 8 slaves from sample JSON",
          "[master][sim][point_generator][contract]") {
    auto pt = makeLoadedTable();
    REQUIRE(pt->slaveCount() == 8u);                // sample 8 簇
    REQUIRE(pt->pointCount() == 43u);               // 教学版 43 个测点
    REQUIRE(pt->allSlaveIds().size() == 8u);

    // 验证 findByName 工作
    const SimPoint* p = pt->findByName("Rack-01_MaxTemp");
    REQUIRE(p != nullptr);
    REQUIRE(p->slaveAddress == 1);
    REQUIRE(p->registerAddr == 0x1000);
}

TEST_CASE("sim_config: defaultsFullTopology gives 23-slave topology (16 BMS + 4 PCS + 3 aux)",
          "[master][sim][point_generator][sim_config]") {
    auto cfg = SimConfig::defaultsFullTopology(/*seed=*/42);
    REQUIRE(cfg.slaves.size() == 23u);
    REQUIRE(cfg.seed == 42u);
    REQUIRE(cfg.tickMs == 100u);
    int bms = 0, pcs = 0, aux = 0;
    for (const auto& s : cfg.slaves) {
        if (s.kind == DeviceKind::Bms)    ++bms;
        if (s.kind == DeviceKind::Pcs)    ++pcs;
        if (s.kind == DeviceKind::Liquid || s.kind == DeviceKind::Fire
            || s.kind == DeviceKind::Meter) ++aux;
    }
    REQUIRE(bms == 16);
    REQUIRE(pcs == 4);
    REQUIRE(aux == 3);
}
