// src/protocol/PointTable.h
// L2 协议引擎 ── 点表解析器（ENS-LLD-100 §4.4 / ENS-DEV-GUIDE §3A 3.1.5）。
//
// 责任边界：
//   * 加载 ptgen.py 生成的 sim_pointtable_sample.json（开发期）或 sim_pointtable_full.json（压测期）。
//   * 提供两类查询：
//       - resolve(slaveAddress, registerAddr) → PointRuntime
//         主程序 ModbusEngine 解析出响应后,按 (slave, addr) 寻址对应测点；
//         与 Track B B5（device_simulator 的 RegisterBank）共享**同一份 JSON**,
//         字节级一致（DevGuide §2C: 两轨点表必须同一文件）。
//       - pointIdOf(pointId) → PointRuntime
//         反向查询,用于数据中枢/UI 按 pointId 寻原始定义（缩放/字节序/单位）。
//   * 寄存器大端拼装 + 字节序重组（§4.4.2 字节序支持 ABCD/CDAB/BADC/DCBA）。
//   * 工程值还原：raw register value → engineering value（含 scaleFactor + offset）。
//
// 不做（Phase 3+ 收口）：
//   * 热加载（reload / 原子切换 snapshot）。当前加载期固化。
//   * 反向 Modbus byte stream → register values（属 ModbusEngine 3.1.3 职责）。
//
// ⚠ 与 apps/device_simulator/src/core/modbus_types.h 的 PointTableEntry 字段一致,
//   但不共享编译单元（DevGuide §2.1 决策：物理拷贝、零依赖）。

#pragma once

#include <cstddef>
#include <cstdint>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ens::protocol {

/// Modbus 寄存器类型（与 LLD §4.4 一致,与 sim 侧 modbus_types.h 字段对齐）。
enum class RegisterType : uint8_t {
    Coil           = 0,   // 0x  线圈
    DiscreteInput  = 1,   // 1x  离散输入
    HoldingRegister = 2,  // 4x  保持寄存器（最常用）
    InputRegister   = 3,  // 3x  输入寄存器
};

/// 数据类型（与 LLD §4.4 一致）。
enum class DataType : uint8_t {
    Bool    = 0,
    Int16   = 1,
    Uint16  = 2,
    Int32   = 3,
    Float32 = 4,
    Float64 = 5,
};

/// 字节序（与 LLD §4.4.1 一致,大端字序 ABCD 为 Modbus 默认）。
enum class ByteOrder : uint8_t {
    ABCD = 0,   // 大端字序 / 大端字节（Modbus 默认）
    BADC = 1,   // 每个 16-bit word 内字节交换
    CDAB = 2,   // dword 内 16-bit word 序反转（32-bit 字序小端化）
    DCBA = 3,   // 完全反转
};

/// 单个测点的运行时契约（不可变：构造后字段不再修改,适合 read-mostly 查表）。
/// @design-intent 与 Phase 4.x 的 L1SnapshotStore::write 热路径共享本结构（zero-copy 友好）。
struct PointRuntime {
    uint32_t    pointId         = 0;
    uint32_t    linkId          = 0;          // 当前约定与 slaveAddress 一致（ptgen.py §entry 注释）
    uint8_t     slaveAddress    = 0;          // 1~247
    RegisterType regType        = RegisterType::HoldingRegister;
    DataType    dataType        = DataType::Uint16;
    ByteOrder   byteOrder       = ByteOrder::ABCD;
    uint16_t    registerAddr    = 0;          // 从站作用域偏移（绝对地址,与 BMS_BASE 等公式无关）
    float       scaleFactor     = 1.0f;
    float       offset          = 0.0f;
    uint32_t    pollIntervalMs  = 1000;
    uint8_t     priority        = 1;
    bool        enabled         = true;
    std::string pointName;
    std::string unit;
};

/// PointTable 契约查询的"找不到"语义：resolve/pointIdOf 返回 nullptr（零拷贝,无 string 拷贝）；
/// allOnSlave 返回空 vector。加载失败抛 std::runtime_error（构造期硬错误,运行期零异常）。
class PointTable {
public:
    PointTable() = default;

    /// 从 JSON 文件加载；schemaVersion 校验在内部完成,失败抛 std::runtime_error。
    /// 推荐用 std::filesystem::path 重载（保留原始 Unicode 路径,不强制 ANSI 转换）。
    /// 字符串重载仅限 ASCII 路径(Windows 中文路径会抛 system_error)。
    static std::shared_ptr<PointTable> loadFromJsonFile(const std::string& path);
    static std::shared_ptr<PointTable> loadFromJsonFile(const std::filesystem::path& path);

    /// 主查询：(slaveAddress, registerAddr) → 指向内部存储的只读 PointRuntime。
    /// 不存在返回 nullptr。指针在 PointTable 生命周期内稳定(表不可变)。
    const PointRuntime* resolve(uint8_t slaveAddress, uint16_t registerAddr) const noexcept;

    /// 反向查询：pointId → 指向内部存储的只读 PointRuntime。
    /// 不存在返回 nullptr。
    const PointRuntime* pointIdOf(uint32_t pointId) const noexcept;

    /// 列出某从站的所有 PointRuntime（按 registerAddr 升序;加载期索引,零扫描）。
    /// 用于 PollScheduler 批量组帧：一次 Modbus 读多寄存器。
    std::vector<const PointRuntime*> allOnSlave(uint8_t slaveAddress) const noexcept;

    /// 数据类型对应的寄存器寄存器数（1 register = 2 bytes）。
    /// 用于 ModbusEngine 计算一次 FC03/FC04 读取应发的 quantity。
    ///   Bool    = 1（coil/discrete 按位打包,FC01/02 的 quantity 语义由调用方换算）
    ///   Int16   = 1
    ///   Uint16  = 1
    ///   Int32   = 2
    ///   Float32 = 2
    ///   Float64 = 4
    static size_t registerCountFor(DataType dt) noexcept;

    /// 寄存器字节序重组：把 N 个 uint16_t 按 byteOrder 重排成字节序列。
    /// 用于 Phase 3.x L1SnapshotStore 在 ModbusEngine 解出响应后,按 PointRuntime
    /// 把 Modbus 默认大端字节流重组为端序指定的工程值。
    /// @return 写入 out 的字节数（= dataType 的字节宽度）。
    static size_t reassembleBytes(const uint16_t* regs, size_t regCount,
                                  ByteOrder order, uint8_t* out, size_t outCap) noexcept;

    /// 工程值还原：把按字节序重排后的字节流解释为浮点,再套 scaleFactor + offset。
    /// 当前仅覆盖 Int16/Uint16/Int32/Float32/Float64；Bool 走 coil 位图,
    /// 由 ModbusEngine (3.1.3) 在 FC01/02 路径单独处理。
    double decodeToEngineering(const uint8_t* bytes, size_t len,
                              DataType dt, float scale, float offset) const noexcept;

    size_t size() const noexcept { return m_byPointId.size(); }

private:
    std::unordered_map<uint32_t, PointRuntime> m_byPointId;     // pointId → runtime
    std::unordered_map<uint64_t, uint32_t>      m_byAddr;       // key=(slave<<32)|addr → pointId
    std::unordered_map<uint8_t, std::vector<uint32_t>> m_bySlave;  // slave → pointId(按 registerAddr 升序)
    static constexpr uint64_t kAddrKey(uint8_t slave, uint16_t addr) noexcept {
        return (static_cast<uint64_t>(slave) << 32) | static_cast<uint64_t>(addr);
    }
};

}  // namespace ens::protocol
