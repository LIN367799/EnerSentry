// core/crc16.cpp —— CRC-16/MODBUS 查表实现（ENS-SIM-IMP §2.2）。
// 与主程序 ENS-LLD-100 §4.1.2 算法逐字节一致：constexpr 编译期生成 256 项表，
// 运行期 (crc>>8) ^ table[(crc^byte)&0xFF]，零运行期初始化开销。
#include "core/crc16.h"

#include <array>

namespace ens::core {

namespace {

constexpr uint16_t tableEntry(uint8_t index) {
    uint16_t crc = index;
    for (int i = 0; i < 8; ++i)
        crc = (crc & 0x0001) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001u)
                             : static_cast<uint16_t>(crc >> 1);
    return crc;
}

constexpr std::array<uint16_t, 256> kTable = [] {
    std::array<uint16_t, 256> t{};
    for (int i = 0; i < 256; ++i) t[static_cast<size_t>(i)] = tableEntry(static_cast<uint8_t>(i));
    return t;
}();

}  // namespace

uint16_t crc16_modbus(const uint8_t* data, size_t len) noexcept {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i)
        crc = static_cast<uint16_t>((crc >> 8) ^ kTable[(crc ^ data[i]) & 0xFF]);
    return crc;
}

}  // namespace ens::core