// src/sim/sim_config.h
// 测试台配置 ── Phase 1 Track B B5（ENS-LLD-SIM §2.2.1, ENS-SIM-IMP §3/§5, ENS-HLD-SIM §4）。
//
// 职责：
//   - 装载运行时配置(TCP/RTU 端点、tickMs、seed、Slaves 拓扑、pointtablePath)
//   - 提供给 SimulatorEngine 作为构造参数(尚未接入 Engine 时由 PointGenerator 单测消费)
//   - JSON schema 与 SIM-IMP §5.1 同(`config/pointtable.json` 兼容)

#pragma once

#include <cstdint>

#include <filesystem>
#include <string>
#include <vector>

#include "core/point_table.h"

namespace ens::sim {

// ─────────────────────────────────────────────────────────────────────────────
// 单从站规格（LLD-SIM §4.4 / HLD-SIM §3.4）：拓扑层最小子集
// ─────────────────────────────────────────────────────────────────────────────
struct SlaveSpec {
    uint8_t       slaveId   = 0;     // 1..23(BMS 1..16 / PCS 17..20 / Meter 21 / Liquid 22 / Fire 23)
    DeviceKind    kind      = DeviceKind::Bms;
    Transport     transport = Transport::Tcp;   // tcp / rtu 二选一
    uint16_t      regCount  = 0;                // HoldingRegister 容量(给 builder.initializeSlave)
};

// ─────────────────────────────────────────────────────────────────────────────
// 端点配置
// ─────────────────────────────────────────────────────────────────────────────
struct TcpEndpoint {
    bool        enabled = true;
    std::string bindIp  = "127.0.0.1";
    uint16_t    port    = 5020;
};

struct RtuEndpoint {
    bool        enabled = true;
    std::string dev     = "COM4";      // 或 /dev/ttyUSB0(POSIX)
    uint32_t    baudRate = 115200;
};

// ─────────────────────────────────────────────────────────────────────────────
// 总体配置（LLD-SIM §4.4 完整定义；Phase 1 Track B 最小子集）
// ─────────────────────────────────────────────────────────────────────────────
struct SimConfig {
    TcpEndpoint            tcp;
    RtuEndpoint            rtu;
    uint32_t               tickMs       = 100;       // SIM-IMP §4.4
    uint32_t               seed         = 0;         // NFR-TEST-01 确定性
    std::vector<SlaveSpec> slaves;
    std::string            pointtablePath = "config/pointtable.json";

    /// 从 JSON 文件加载（兼容 SIM-IMP §3.2 schema）。
    /// 找不到文件或 schemaVersion 不匹配时,抛 std::runtime_error。
    static SimConfig loadFromJsonFile(const std::filesystem::path& path);

    /// 默认 23 从站拓扑（SIM-IMP §3.1）—— Phase 1 Track B 暂不接 16 簇 20800 全量，
    /// 教学版 sample.json 是 8 从站,由 PointGenerator 内部 init。
    /// 此处保留 23 拓扑便于点表加载阶段按 slaveId 索引 register bank。
    static SimConfig defaultsFullTopology(uint32_t seed);

    /// 从 SimPointTable(samples 已加载)推导 Slaves：所见即所建 register bank
    static std::vector<SlaveSpec> fromPointTable(const SimPointTable& pt);
};

}  // namespace ens::sim
