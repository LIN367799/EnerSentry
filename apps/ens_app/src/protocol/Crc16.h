// src/protocol/Crc16.h
// L2 协议引擎 ── CRC-16/MODBUS 查表加速（ENS-LLD-100 §4.1.2 / ADR-LLD-11）。
// 多项式 0xA001（反射），初值 0xFFFF；运行期单次查表 + 异或，热路径 O(len)。
//
// ⚠ 与 apps/device_simulator/src/core/crc16.cpp 命名/算法/字节序一致：
//    落帧时低字节在前（crc & 0xFF, crc >> 8）。Tier 3 联调用例用作"两轨 CRC 方言等价"的硬证据。
//    （DevGuide §2C 用例实证，避免测试台响应发出去主程序校验不过）
//
// 设计要点：
//   - 全部 `constexpr` 在编译期生成 256 项表，零运行期初始化；
//   - header-only inline，消费 TU 直接 #include，无需 .cpp 链接（CMS 仍提供 Crc16.cpp 做
//     "层门面 TU"，编译期断言 sizeof 表 == 512，防止未来误改）。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ens::protocol {

/// @brief CRC-16/MODBUS 单表项生成（反射多项式 0xA001，初值即 index）
constexpr uint16_t crc16ModbusEntry(uint8_t index) noexcept {
    uint16_t crc = index;
    for (int i = 0; i < 8; ++i)
        crc = (crc & 0x0001) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001u)
                             : static_cast<uint16_t>(crc >> 1);
    return crc;
}

/// 256 项查表（编译期生成，零运行期初始化）。
/// static constexpr 数据成员：避免头被多个 TU include 时引发的 ODR 重复定义风险，
/// 所有 TU 共享同一份表（链接器 dedupe）。
inline constexpr std::array<uint16_t, 256> kCrc16ModbusTable = [] {
    std::array<uint16_t, 256> t{};
    for (int i = 0; i < 256; ++i)
        t[static_cast<size_t>(i)] = crc16ModbusEntry(static_cast<uint8_t>(i));
    return t;
}();

/// 计算 data[0..len) 的 CRC-16/MODBUS 原始 16-bit 值。
/// ⚠ 落帧时按"低字节在前"：buf[end] = static_cast<uint8_t>(crc & 0xFF);
///                       buf[end+1] = static_cast<uint8_t>(crc >> 8);
/// 入参：data == nullptr 且 len == 0 时返回初值 0xFFFF（与 dev_simulator 行为一致）。
inline uint16_t crc16Modbus(const uint8_t* data, size_t len) noexcept {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i)
        crc = static_cast<uint16_t>((crc >> 8) ^ kCrc16ModbusTable[(crc ^ data[i]) & 0xFF]);
    return crc;
}

/// RTU 帧内嵌 CRC 校验：frame[0..totalLen-2) 是 PDU，frame[totalLen-2]/[-1] 是低/高字节。
/// totalLen < 4（至少 addr+fc+crc）视为不合法，返回 false。
inline bool crc16ModbusVerify(const uint8_t* frame, size_t totalLen) noexcept {
    if (frame == nullptr || totalLen < 4) return false;
    const uint16_t calc = crc16Modbus(frame, totalLen - 2);
    const uint16_t recv = static_cast<uint16_t>(frame[totalLen - 2])
                        | static_cast<uint16_t>(frame[totalLen - 1] << 8);
    return calc == recv;
}

// ── 双字节查表变体（ADR-LLD-11，可选加速；x86-64 与单表版性能相当，按需启用）──
inline constexpr std::array<uint8_t, 256> kCrcHi = [] {
    std::array<uint8_t, 256> t{};
    for (int i = 0; i < 256; ++i)
        t[static_cast<size_t>(i)] = static_cast<uint8_t>(crc16ModbusEntry(static_cast<uint8_t>(i)) >> 8);
    return t;
}();
inline constexpr std::array<uint8_t, 256> kCrcLo = [] {
    std::array<uint8_t, 256> t{};
    for (int i = 0; i < 256; ++i)
        t[static_cast<size_t>(i)] = static_cast<uint8_t>(crc16ModbusEntry(static_cast<uint8_t>(i)) & 0xFF);
    return t;
}();

}  // namespace ens::protocol
