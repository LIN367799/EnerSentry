// src/protocol/Crc16.cpp
// L2 协议引擎 ── Crc16 模块"门面 TU"（ENS-LLD-100 §4.1.2）。
//
// 作用：
//   ① 编译期强制校验：kCrc16ModbusTable / kCrcHi / kCrcLo 仍为正确尺寸（512 / 256 / 256 字节）。
//      一旦未来误改模板,静态断言直接失败,无需等运行时校验出问题。
//   ② 把所有 constexpr 表拉到这一个 .o 中集中"具现化" → libens_protocol.a 的 CRC 段不会
//      因各 TU 各自持有 inline constexpr 副本而膨胀；多 TU include 时表共享同一份链接符号。
//   ③ 暴露 layerName() 供日志/诊断模块识别协议层版本。
//
// 真实算法（Crc16Modbus / Crc16Verify / 双字节变体）全部 inline 在 Crc16.h，
// 消费者直接 #include "protocol/Crc16.h" 取值。

#include "Crc16.h"

namespace ens::protocol {

// 编译期闸：表尺寸与字段宽度永远等于设计值（防御性 C++17 静态断言）。
static_assert(sizeof(kCrc16ModbusTable) == sizeof(uint16_t) * 256,
              "kCrc16ModbusTable must be 256 * 2 = 512 bytes");
static_assert(sizeof(kCrcHi) == 256, "kCrcHi must be 256 bytes");
static_assert(sizeof(kCrcLo) == 256, "kCrcLo must be 256 bytes");
static_assert(kCrc16ModbusTable.size() == 256u, "CRC16 table size regression");
static_assert(crc16ModbusEntry(0x00) == 0x0000u,
              "CRC16 entry(0x00) must be 0 (sanity for reflected polynomial)");

// 层锚：日志/诊断据此识别当前协议方言版本（与 dev_simulator src/core 同名约定对齐）。
const char* crc16LayerName() noexcept { return "ens::protocol::crc16modbus-v1"; }

}  // namespace ens::protocol
