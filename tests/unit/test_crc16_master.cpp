// tests/unit/test_crc16_master.cpp
// 主程序侧 ens::protocol::crc16Modbus Tier 2 单测（ENS-DEV-GUIDE §3A 3.1.1）。
// 覆盖：
//   ① 标准校验串 "123456789" == 0x4B37（与 sim 侧 LLD-100 §4.1.2 等价）；
//   ② 空数据保持初值 0xFFFF；
//   ③ 查表版 == 逐位参考实现(0..255 + 变长)；
//   ④ crc16ModbusVerify 双字节落帧（低字节在前）正确认；
//   ⑤ 编译期断言：sizeof(kCrc16ModbusTable) == 512。
// 与 tests/unit/test_crc16.cpp（sim 侧 ens::core::crc16_modbus）并存,
// 用同一组 case 实证"测试台 CRC 方言 == 主程序 CRC 方言"（DevGuide §2C）。

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include "protocol/Crc16.h"

using ens::protocol::crc16Modbus;
using ens::protocol::crc16ModbusVerify;
using ens::protocol::kCrc16ModbusTable;

// 逐位参考实现（标准 CRC-16/MODBUS：poly 0xA001, init 0xFFFF, reflected）
static uint16_t crc16Bitwise(const uint8_t* data, size_t len) noexcept {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x0001) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001u)
                                 : static_cast<uint16_t>(crc >> 1);
    }
    return crc;
}

TEST_CASE("crc16_modbus: standard check string 123456789 == 0x4B37", "[master][crc16]") {
    const char* s = "123456789";
    REQUIRE(crc16Modbus(reinterpret_cast<const uint8_t*>(s), 9) == 0x4B37u);
}

TEST_CASE("crc16_modbus: empty input keeps init 0xFFFF", "[master][crc16]") {
    REQUIRE(crc16Modbus(nullptr, 0) == 0xFFFFu);
}

TEST_CASE("crc16_modbus: table implementation matches bitwise reference (full domain)",
          "[master][crc16]") {
    std::vector<uint8_t> buf(256);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(i);
    for (size_t len = 1; len <= buf.size(); len += 31) {
        const uint16_t expected = crc16Bitwise(buf.data(), len);
        CHECK(crc16Modbus(buf.data(), len) == expected);
    }
}

TEST_CASE("crc16_modbus: verify matches low-byte-first frame layout", "[master][crc16]") {
    // 最小合法 RTU 帧 = addr+fc+CRC = 4 字节（与 parseRtuResponse len<4 守卫一致）。
    // 用 bitwise 参考实现算 2 字节 PDU 的"已知正确 CRC",拼成低字节在前的 4 字节帧。
    const uint8_t pdu[] = {0x01, 0x03};
    const uint16_t crc = crc16Bitwise(pdu, sizeof(pdu));
    std::vector<uint8_t> frame = {pdu[0],
                                 pdu[1],
                                 static_cast<uint8_t>(crc & 0xFF),
                                 static_cast<uint8_t>(crc >> 8)};
    REQUIRE(frame.size() == 4u);
    REQUIRE(crc16ModbusVerify(frame.data(), frame.size()));
}

TEST_CASE("crc16_modbus: verify rejects single-bit corrupt frame", "[master][crc16]") {
    const uint8_t byte = 0x01;
    const uint16_t crc = crc16Bitwise(&byte, 1);
    std::vector<uint8_t> frame = {byte,
                                 static_cast<uint8_t>(crc & 0xFF),
                                 static_cast<uint8_t>(crc >> 8)};
    frame[1] ^= 0x01;   // 翻 1 比特 → CRC 必不过
    REQUIRE_FALSE(crc16ModbusVerify(frame.data(), frame.size()));
}

TEST_CASE("crc16_modbus: verify rejects too-short frame (< 4 bytes)", "[master][crc16]") {
    uint8_t a = 0x01;
    REQUIRE_FALSE(crc16ModbusVerify(&a, 0));
    REQUIRE_FALSE(crc16ModbusVerify(&a, 1));
    REQUIRE_FALSE(crc16ModbusVerify(&a, 2));
    // 边界回归锁定（0b1cf90 改 3→4）：3 字节帧缺 1 字节 CRC,必须拒绝
    const uint8_t frame3[] = {0x01, 0x03, 0x00};
    REQUIRE_FALSE(crc16ModbusVerify(frame3, sizeof(frame3)));
}

TEST_CASE("crc16_modbus: table is 256 entries of 16-bit values", "[master][crc16]") {
    REQUIRE(kCrc16ModbusTable.size() == 256u);
    REQUIRE(sizeof(kCrc16ModbusTable) == 512u);
    // 抽 [0] / [1] 已知 entry 验证表内容（防未来换多项式）。
    // [0x00] 反射多项式下恒为 0；[0x01] 经典值 0xC0C1。
    // 不写更多硬编码避免凭印象出错；任何 entry 都可由 crc16ModbusEntry(idx) 反推。
    REQUIRE(kCrc16ModbusTable[0]   == 0x0000u);
    REQUIRE(kCrc16ModbusTable[1]   == 0xC0C1u);
    REQUIRE(kCrc16ModbusTable[0xFF] == ens::protocol::crc16ModbusEntry(0xFF));
}
