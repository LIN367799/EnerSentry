// tests/unit/test_accumulator.cpp
// ModbusStreamAccumulator Tier 2 单测（ENS-DEV-GUIDE §2A 2.1.4）。
// 覆盖：碎片化提帧 / 双帧拼接 / 异常帧 5B·9B 定长 / 溢出覆盖恢复 / TCP Length 切分。
// 注：RTU 帧含真实 CRC-16/MODBUS（poly 0xA001, init 0xFFFF, 低字节在前）。

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <vector>
#include "ModbusStreamAccumulator.h"

using ens::protocol::ModbusStreamAccumulator;

// ---- 测试辅助：CRC-16/MODBUS 与 RTU 读响应帧构造 ----
static uint16_t crc16Modbus(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x0001)
                ? static_cast<uint16_t>((crc >> 1) ^ 0xA001)
                : static_cast<uint16_t>(crc >> 1);
    }
    return crc;
}

static std::vector<uint8_t> makeRtuReadResp(uint8_t slave, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> f = {slave, 0x03, static_cast<uint8_t>(data.size())};
    f.insert(f.end(), data.begin(), data.end());
    const uint16_t crc = crc16Modbus(f.data(), f.size());
    f.push_back(static_cast<uint8_t>(crc & 0xFF));
    f.push_back(static_cast<uint8_t>(crc >> 8));
    return f;
}

TEST_CASE("Accumulator: one frame split into 5 packets yields exactly 1 complete frame", "[protocol][accumulator]") {
    ModbusStreamAccumulator acc;
    const auto frame = makeRtuReadResp(0x11, {0x00, 0x0A});  // 7 字节 RTU 读响应
    REQUIRE(frame.size() == 7);

    for (uint8_t b : frame) acc.append(&b, 1);  // 模拟碎片化到达

    uint8_t out[256];
    size_t len = 0;
    REQUIRE(acc.tryExtractFrame(out, len, /*isTcp=*/false));
    REQUIRE(len == frame.size());
    REQUIRE(std::memcmp(out, frame.data(), len) == 0);

    uint8_t out2[256];
    size_t len2 = 0;
    REQUIRE_FALSE(acc.tryExtractFrame(out2, len2, false));  // 无第二帧
}

TEST_CASE("Accumulator: two concatenated frames yield 2 frames", "[protocol][accumulator]") {
    ModbusStreamAccumulator acc;
    const auto f1 = makeRtuReadResp(0x11, {0x00, 0x0A});
    const auto f2 = makeRtuReadResp(0x12, {0x01, 0x02, 0x03});
    std::vector<uint8_t> both = f1;
    both.insert(both.end(), f2.begin(), f2.end());
    acc.append(both.data(), both.size());

    uint8_t out[256];
    size_t len = 0;
    REQUIRE(acc.tryExtractFrame(out, len, false));
    REQUIRE(len == f1.size());
    REQUIRE(std::memcmp(out, f1.data(), len) == 0);

    REQUIRE(acc.tryExtractFrame(out, len, false));
    REQUIRE(len == f2.size());
    REQUIRE(std::memcmp(out, f2.data(), len) == 0);

    REQUIRE_FALSE(acc.tryExtractFrame(out, len, false));  // 无第三帧
}

TEST_CASE("Accumulator: exception frame (func|0x80) extracted by fixed 5B/9B length", "[protocol][accumulator]") {
    // RTU 异常帧：slave + 0x83 + excCode + CRC = 5 字节（含真实 CRC）
    {
        ModbusStreamAccumulator acc;
        std::vector<uint8_t> f = {0x11, 0x83, 0x02};
        const uint16_t crc = crc16Modbus(f.data(), f.size());
        f.push_back(static_cast<uint8_t>(crc & 0xFF));
        f.push_back(static_cast<uint8_t>(crc >> 8));
        acc.append(f.data(), f.size());

        uint8_t out[16];
        size_t len = 0;
        REQUIRE(acc.tryExtractFrame(out, len, false));
        REQUIRE(len == 5);
        REQUIRE(out[0] == 0x11);
        REQUIRE((out[1] & 0x80) != 0);
    }
    // TCP 异常帧：MBAP(7) + func|0x80 + excCode = 9 字节
    {
        ModbusStreamAccumulator acc;
        uint8_t f[9] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x11, 0x83, 0x02};
        acc.append(f, sizeof(f));

        uint8_t out[16];
        size_t len = 0;
        REQUIRE(acc.tryExtractFrame(out, len, /*isTcp=*/true));
        REQUIRE(len == 9);
        REQUIRE((out[7] & 0x80) != 0);  // MBAP 后首字节为 func|0x80
    }
}

TEST_CASE("Accumulator: overflow overwrite no crash and m_read advances (V1.2)", "[protocol][accumulator]") {
    ModbusStreamAccumulator acc;
    // 0x00 垃圾：非异常帧(最高位 0)且 CRC 必失败 → 走 HUNT 前滑（0xAA 会误判异常帧）
    std::vector<uint8_t> junk(ModbusStreamAccumulator::kCapacity + 100, 0x00);
    acc.append(junk.data(), junk.size());

    uint8_t out[256];
    size_t len = 0;
    REQUIRE_FALSE(acc.tryExtractFrame(out, len, false));

    // 溢出后合法帧落在环形"尾部"，需模拟逐字节 HUNT 前滑（最多 kCapacity 次）找到
    const auto frame = makeRtuReadResp(0x11, {0x00, 0x0A});
    acc.append(frame.data(), frame.size());
    bool extracted = false;
    for (int i = 0; i < static_cast<int>(ModbusStreamAccumulator::kCapacity) && !extracted; ++i) {
        extracted = acc.tryExtractFrame(out, len, false);
    }
    REQUIRE(extracted);
    REQUIRE(len == frame.size());
    REQUIRE(std::memcmp(out, frame.data(), len) == 0);
}

TEST_CASE("Accumulator: TCP MBAP frame sliced by Length field", "[protocol][accumulator]") {
    ModbusStreamAccumulator acc;
    // transId=0x0001, proto=0, length=0x0005(UnitId 0x11 + PDU: 03 02 000A)
    uint8_t f[11] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x11, 0x03, 0x02, 0x00, 0x0A};
    acc.append(f, sizeof(f));

    uint8_t out[32];
    size_t len = 0;
    REQUIRE(acc.tryExtractFrame(out, len, /*isTcp=*/true));
    REQUIRE(len == 11);
    REQUIRE(out[7] == 0x03);   // PDU 首字节 = Function
    REQUIRE(out[10] == 0x0A);  // 末字节 = Data 低位
}
