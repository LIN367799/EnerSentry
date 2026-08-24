// core/crc16.h —— 测试台公共契约：CRC-16/MODBUS（ENS-SIM-IMP §2.2）。
// poly 0xA001, init 0xFFFF, 低字节在前（与主程序 HLD-COMM / LLD-100 §4.1.2 方言一致）。
// 纯 C++17，零依赖，可在无 Qt 环境独立编译。
#pragma once

#include <cstddef>
#include <cstdint>

namespace ens::core {

// 计算 data[0..len) 的 CRC-16/MODBUS（返回原始 16-bit 值；落帧时低字节在前：crc&0xFF, crc>>8）
uint16_t crc16_modbus(const uint8_t* data, size_t len) noexcept;

}  // namespace ens::core