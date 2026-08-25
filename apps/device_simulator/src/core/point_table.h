// src/core/point_table.h
// 测试台点表共享契约 ── Phase 1 Track B B5 入口（ENS-SIM-IMP §3, ENS-LLD-SIM §2.2.1）。
//
// 与主程序侧 src/protocol/PointTable.h 的关系：
//   - **byte-by-byte 等价**（同一份 sim_pointtable_sample.json，两侧解析后字段、枚举值、
//     scaleFactor、offset、byteOrder 全部一致）是 ENS-LLD-100 §3.1 隐约定：测试台仿真的
//     raw 值必须能被主程序 encodeToWire/decodeToEngineering 正确解读。
//   - 但**独立编译**：device_simulator target 不依赖 main app（ENS-DEV-ARCH §2.1），
//     所以这里 type/load 是 src/core 内部的最小子集，**仅用于 PointGenerator 物理演化器**，
//     不暴露 ModbusFrame 组拆帧 / 字节序重组等主程序关注的功能。

#pragma once

#include <cstddef>
#include <cstdint>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ens::sim {

// ─────────────────────────────────────────────────────────────────────────────
// 寄存器类（与 HLD-SIM §3.4 / SIM-IMP §2.2 一致）
// ─────────────────────────────────────────────────────────────────────────────
enum class RegisterType : uint8_t {
    HoldingRegister = 0,
    InputRegister   = 1,
    Coil            = 2,
    DiscreteInput   = 3,
};

// ─────────────────────────────────────────────────────────────────────────────
// 数据类型（与主程序 PointTable::DataType 一一对应；sim 物理模型必须按此选择容器）
// ─────────────────────────────────────────────────────────────────────────────
enum class DataType : uint8_t {
    Bool   = 0,
    Int16  = 1,
    Uint16 = 2,
    Int32  = 3,
    Float32 = 4,
    Float64 = 5,
};

// ─────────────────────────────────────────────────────────────────────────────
// 字节序（sim 侧不重组字节，IO 线程按主程序 ModbusFrame 重组 — 物理模型不需要关心）
// 仅落地、保存语义。
// ─────────────────────────────────────────────────────────────────────────────
enum class ByteOrder : uint8_t {
    ABCD = 0,
    BADC = 1,
    CDAB = 2,
    DCBA = 3,
};

// ─────────────────────────────────────────────────────────────────────────────
// 设备类别（用于 PointGenerator 决定推哪一种 evolveXxx）
// ─────────────────────────────────────────────────────────────────────────────
enum class DeviceKind : uint8_t {
    Bms   = 0,
    Pcs   = 1,
    Meter = 2,
    Liquid = 3,    // Liquid=液冷 = aux_A 类
    Fire  = 4,
};

// ─────────────────────────────────────────────────────────────────────────────
// 链路归属（与 HLD-SIM §3.6 / SIM-IMP §6.2 一致）：
//   Tcp = TCP 5020(承载 BMS/PCS/Meter),Rtu = com0com 虚拟串口(承载辅机)
// ─────────────────────────────────────────────────────────────────────────────
enum class Transport : uint8_t {
    Tcp = 0,
    Rtu = 1,
};

// ─────────────────────────────────────────────────────────────────────────────
// 单个点（仅 sim 物理演化器关注的字段；与主程序 PointRuntime 对齐）
// ─────────────────────────────────────────────────────────────────────────────
struct SimPoint {
    uint32_t     pointId       = 0;
    std::string  pointName;
    uint8_t      slaveAddress  = 0;
    RegisterType regType       = RegisterType::HoldingRegister;
    DataType     dataType      = DataType::Float32;
    ByteOrder    byteOrder     = ByteOrder::ABCD;
    uint16_t     registerAddr  = 0;
    float        scaleFactor   = 1.0f;
    float        offset        = 0.0f;
    std::string  unit;
    bool         enabled       = true;
};

// ─────────────────────────────────────────────────────────────────────────────
// 每从站最小子集（PointGenerator 决定 evolveXxx 用）
// ─────────────────────────────────────────────────────────────────────────────
struct SlaveTable {
    uint8_t                       slaveId      = 0;
    DeviceKind                    kind         = DeviceKind::Bms;
    Transport                     transport    = Transport::Tcp;
    std::vector<SimPoint>         points;                  // 本从站包含的全部点
};

// ─────────────────────────────────────────────────────────────────────────────
// SimPointTable —— 加载 sim_pointtable_sample.json 后按 slaveAddress 分组
// 用于：PointGenerator 初始化 23(SIM-IMP §3.1) 或 sample 版 8(SIM 教学版)
// 从站 SlaveRegset 容器大小；按 pointName 索引常用字段
// ─────────────────────────────────────────────────────────────────────────────
class SimPointTable {
public:
    SimPointTable() = default;
    SimPointTable(const SimPointTable&) = delete;
    SimPointTable& operator=(const SimPointTable&) = delete;
    SimPointTable(SimPointTable&&) = default;
    SimPointTable& operator=(SimPointTable&&) = default;

    /// 从 JSON 文件加载(sim_pointtable_sample.json),失败抛 std::runtime_error
    /// schemaVersion 校验在内部完成(目前仅 1.0/1.1 接受)
    /// ⚠ 与主程序 PointTable 走相同路径策略:走 _wfopen / _waccess 绕 MSVC
    ///   std::filesystem 中文 path + system code page 三坑（Phase 2 3.1.5 实测坑 #6）
    static std::shared_ptr<SimPointTable> loadFromJsonFile(
        const std::filesystem::path& path);

    /// 按 slaveAddress 查一组点（无序返回）
    const std::vector<SimPoint>& onSlave(uint8_t slave) const noexcept;

    /// O(1) 反查：通过 pointName
    const SimPoint* findByName(const std::string& name) const noexcept;

    /// 全部从站点数（用于 PointGenerator 初始化 RegisterBank 时建仓数量）
    size_t slaveCount() const noexcept { return m_bySlave.size(); }

    /// 全部点数（诊断 / NFR-TEST-02 日志用）
    size_t pointCount() const noexcept;

    /// 全部 slave 列表（PointGenerator 顺序遍历用）
    std::vector<uint8_t> allSlaveIds() const noexcept;

    /// 给定 slave 上是否有 enable 的 HoldingRegister 类的某 registerAddr（IO 线程解码用）
    bool hasHoldingAt(uint8_t slave, uint16_t addr) const noexcept;

private:
    std::unordered_map<uint8_t, std::vector<SimPoint>> m_bySlave;
    std::unordered_map<std::string, const SimPoint*>  m_byName;  // 非 owning
};

}  // namespace ens::sim
