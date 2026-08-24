// tests/unit/test_crc16.cpp
// ens::core::crc16_modbus Tier 2 单测（ENS-DEV-GUIDE §2B B1）。
// 覆盖：标准校验串 0x4B37 / 空数据保持 init / 与逐位参考实现交叉验证（防查表实现漂移）。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include "core/crc16.h"

using ens::core::crc16_modbus;

// 逐位参考实现（标准 CRC-16/MODBUS：poly 0xA001, init 0xFFFF, reflected）
static uint16_t crc16Bitwise(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x0001) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001u)
                                 : static_cast<uint16_t>(crc >> 1);
    }
    return crc;
}

TEST_CASE("crc16: standard check string 123456789 == 0x4B37", "[core][crc16]") {
    const char* s = "123456789";
    REQUIRE(crc16_modbus(reinterpret_cast<const uint8_t*>(s), 9) == 0x4B37u);
}

TEST_CASE("crc16: empty input keeps init value 0xFFFF", "[core][crc16]") {
    REQUIRE(crc16_modbus(nullptr, 0) == 0xFFFFu);
}

TEST_CASE("crc16: table implementation matches bitwise reference", "[core][crc16]") {
    // 覆盖 0..255 全字节域 + 变长，确保查表版与逐位版逐字节一致
    std::vector<uint8_t> buf(256);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(i);

    for (size_t len = 1; len <= buf.size(); len += 31) {
        const uint16_t expected = crc16Bitwise(buf.data(), len);
        CHECK(crc16_modbus(buf.data(), len) == expected);
    }
}

TEST_CASE("crc16: single byte 0x00 deterministic value", "[core][crc16]") {
    // 与逐位参考一致即可（此处固化该参考值，防未来改动悄悄改变语义）
    const uint8_t one = 0x00;
    REQUIRE(crc16_modbus(&one, 1) == crc16Bitwise(&one, 1));
}