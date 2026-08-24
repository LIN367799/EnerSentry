// core/modbus_types.h —— 测试台公共契约：点表类型（ENS-SIM-IMP §2.2 / ENS-LLD-SIM §4.1）。
// 与主程序 ICD §7.1 类型**逐字节一致**，但物理拷贝、不共享编译单元（§2.1 决策）。
// 注意：registerAddr 是从站作用域偏移（非全局地址），Unit ID（slaveAddress）单独路由。
#pragma once

#include <cstdint>
#include <string>

namespace ens::core {

enum class RegisterType : uint8_t {
    Coil           = 0,   // 0x
    DiscreteInput  = 1,   // 1x
    HoldingRegister = 2,  // 4x（最常用）
    InputRegister  = 3,   // 3x
};

enum class DataType : uint8_t {
    Bool    = 0,
    Int16   = 1,
    Uint16  = 2,
    Int32   = 3,
    Float32 = 4,
    Float64 = 5,
};

// 32-bit 值字节序（大端字序 ABCD 为 Modbus 默认；BADC/CDAB/DCBA 为常见厂商变体）
enum class ByteOrder : uint8_t {
    ABCD  = 0,
    BADC  = 1,
    CDAB  = 2,
    DCBA  = 3,
};

struct PointTableEntry {
    uint32_t    pointId;
    std::string pointName;
    uint32_t    linkId;
    uint8_t     slaveAddress;    // 从站地址（Unit ID），单独路由
    RegisterType regType;
    uint16_t    registerAddr;    // 从站作用域偏移（非全局地址）
    DataType    dataType;
    ByteOrder   byteOrder;
    float       scaleFactor;
    float       offset;
    std::string unit;
    uint32_t    pollIntervalMs;
    uint8_t     priority;
    bool        enabled;
};

}  // namespace ens::core