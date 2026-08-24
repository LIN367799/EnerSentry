// tests/integration/test_modbus_loopback.cpp
// 跨方言 master/slave 回环互逆校验（ENS-DEV-GUIDE §2B B1 / §2C CRC 方言行）。
// master 侧 = 测试台 ens::core（crc16_modbus 造 RTU 帧）；
// slave  侧 = 主程序 ens::protocol（ModbusStreamAccumulator 校验并提取）。
// 二者**不共享编译单元**（core 源码直接编入本测试），此用例实证"双方 CRC 方言一致"。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include "core/crc16.h"                       // master 方言（测试台）
#include "ModbusStreamAccumulator.h"          // slave 方言（主程序）

using ens::core::crc16_modbus;
using ens::protocol::ModbusStreamAccumulator;

// 用测试台方言构造一条合法 RTU 读响应帧：addr + fc=0x03 + 字节数 + 数据 + CRC(低字节在前)
static std::vector<uint8_t> makeRtuReadResp(uint8_t slave, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> f;
    f.push_back(slave);
    f.push_back(0x03);
    f.push_back(static_cast<uint8_t>(payload.size()));
    f.insert(f.end(), payload.begin(), payload.end());
    const uint16_t crc = crc16_modbus(f.data(), f.size());
    f.push_back(static_cast<uint8_t>(crc & 0xFF));       // 低字节在前
    f.push_back(static_cast<uint8_t>(crc >> 8));
    return f;
}

TEST_CASE("loopback: core-built RTU frame extracted intact by protocol accumulator", "[loopback][crc]") {
    const std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    const std::vector<uint8_t> frame   = makeRtuReadResp(1, payload);

    ModbusStreamAccumulator acc;
    acc.append(frame.data(), frame.size());

    std::array<uint8_t, 260> out{};
    size_t outLen = 0;
    REQUIRE(acc.tryExtractFrame(out.data(), outLen, /*isTcp=*/false));
    CHECK(outLen == frame.size());
    REQUIRE(outLen == frame.size());
    CHECK(std::memcmp(out.data(), frame.data(), outLen) == 0);   // 逐字节一致（含 CRC 尾）
}

TEST_CASE("loopback: corrupted CRC rejected across dialects", "[loopback][crc]") {
    std::vector<uint8_t> frame = makeRtuReadResp(2, {0x11, 0x22, 0x33, 0x44});
    frame.back() ^= 0xFF;                                        // 翻转 CRC 高字节 → 假坏帧

    ModbusStreamAccumulator acc;
    acc.append(frame.data(), frame.size());

    std::array<uint8_t, 260> out{};
    size_t outLen = 0;
    CHECK_FALSE(acc.tryExtractFrame(out.data(), outLen, /*isTcp=*/false));
}

TEST_CASE("loopback: two concatenated core-built frames yield two frames", "[loopback][crc]") {
    const auto f1 = makeRtuReadResp(1, {0xAA, 0xBB});
    const auto f2 = makeRtuReadResp(3, {0xCC, 0xDD, 0xEE});

    ModbusStreamAccumulator acc;
    acc.append(f1.data(), f1.size());
    acc.append(f2.data(), f2.size());

    std::array<uint8_t, 260> out{};
    size_t outLen = 0;

    REQUIRE(acc.tryExtractFrame(out.data(), outLen, false));
    CHECK(outLen == f1.size());
    CHECK(std::memcmp(out.data(), f1.data(), outLen) == 0);

    REQUIRE(acc.tryExtractFrame(out.data(), outLen, false));
    CHECK(outLen == f2.size());
    CHECK(std::memcmp(out.data(), f2.data(), outLen) == 0);
}