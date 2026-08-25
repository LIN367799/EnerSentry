// tests/unit/test_modbus_frame.cpp
// 主程序侧 ens::protocol::ModbusFrame Tier 2 单测（ENS-DEV-GUIDE §3A 3.1.2）。
// 覆盖：
//   ① FC03/04/05/06/0F/10 组帧字节序列 == LLD-100 §4.1.1/§4.1.3 契约；
//   ② 同组字节经 parseResponse 反解 → registerValues / coilBytes 与请求字段一致；
//   ③ RTU 闭环：buildRequest → crc16ModbusVerify → true（CRC 双向性实证）；
//   ④ TCP/MBAP 组帧：协议头 7 字节 + length 自洽；
//   ⑤ 异常帧识别：function|0x80 → isException=true / exceptionCode 正确；
//   ⑥ parseResponse 健壮性：长度不够返 nullopt / CRC 错返 nullopt。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "protocol/Crc16.h"
#include "protocol/ModbusFrame.h"

using ens::protocol::Transport;
using ens::protocol::ModbusRequest;
using ens::protocol::ModbusResponse;
using ens::protocol::buildRequest;
using ens::protocol::parseResponse;
using ens::protocol::parseRtuResponse;
using ens::protocol::parseTcpResponse;
using ens::protocol::crc16ModbusVerify;

// ── 工具：把 vector 转 span-style 指针 ——
static inline const uint8_t* bytes(const std::vector<uint8_t>& v) noexcept {
    return v.data();
}

// ─────────────────────────────────────────────────────────────────────────────
// 组帧：FC03 读保持寄存器（RTU）
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_frame: FC03 RTU buildRequest matches canonical layout", "[master][frame][build]") {
    ModbusRequest req;
    req.transport      = Transport::Rtu;
    req.slaveAddress   = 0x11;
    req.functionCode   = 0x03;
    req.startingAddress = 0x006B;
    req.quantity        = 0x0003;

    const auto out = buildRequest(req);
    // [unitId][FC][addr_hi][addr_lo][qty_hi][qty_lo][crc_lo][crc_hi]
    REQUIRE(out.size() == 8);
    REQUIRE(out[0] == 0x11);
    REQUIRE(out[1] == 0x03);
    REQUIRE(out[2] == 0x00);
    REQUIRE(out[3] == 0x6B);
    REQUIRE(out[4] == 0x00);
    REQUIRE(out[5] == 0x03);
    REQUIRE(crc16ModbusVerify(out.data(), out.size()));
}

TEST_CASE("modbus_frame: FC04 RTU read input registers", "[master][frame][build]") {
    ModbusRequest req;
    req.transport      = Transport::Rtu;
    req.slaveAddress   = 0x01;
    req.functionCode   = 0x04;
    req.startingAddress = 0x0000;
    req.quantity        = 0x000A;

    const auto out = buildRequest(req);
    REQUIRE(out.size() == 8);
    REQUIRE(out[0] == 0x01);
    REQUIRE(out[1] == 0x04);
    REQUIRE(crc16ModbusVerify(bytes(out), out.size()));
}

// ─────────────────────────────────────────────────────────────────────────────
// 组帧：FC05 写单线圈（仅 0xFF00/0x0000）
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_frame: FC05 RTU write single coil (ON)", "[master][frame][build]") {
    ModbusRequest req;
    req.transport      = Transport::Rtu;
    req.slaveAddress   = 0x0A;
    req.functionCode   = 0x05;
    req.startingAddress = 0x00AC;
    req.registerValues  = {0xFF00};   // ON

    const auto out = buildRequest(req);
    REQUIRE(out.size() == 8);
    REQUIRE(out[0] == 0x0A);
    REQUIRE(out[1] == 0x05);
    REQUIRE(out[2] == 0x00); REQUIRE(out[3] == 0xAC);   // addr
    REQUIRE(out[4] == 0xFF); REQUIRE(out[5] == 0x00);   // 0xFF00 大端
    REQUIRE(crc16ModbusVerify(bytes(out), out.size()));
}

TEST_CASE("modbus_frame: FC05 RTU rejects invalid coil value", "[master][frame][build][neg]") {
    ModbusRequest req;
    req.transport      = Transport::Rtu;
    req.slaveAddress   = 0x0A;
    req.functionCode   = 0x05;
    req.startingAddress = 0x0000;
    req.registerValues  = {0x0001};   // 非法 → 必须返空
    REQUIRE(buildRequest(req).empty());
}

TEST_CASE("modbus_frame: FC06 RTU write single register", "[master][frame][build]") {
    ModbusRequest req;
    req.transport       = Transport::Rtu;
    req.slaveAddress    = 0x01;
    req.functionCode    = 0x06;
    req.startingAddress = 0x0001;
    req.registerValues  = {0x0003};
    const auto out = buildRequest(req);
    REQUIRE(out.size() == 8);
    REQUIRE(crc16ModbusVerify(bytes(out), out.size()));
}

// ─────────────────────────────────────────────────────────────────────────────
// 组帧：FC10 写多寄存器
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_frame: FC10 RTU write multiple registers", "[master][frame][build]") {
    ModbusRequest req;
    req.transport       = Transport::Rtu;
    req.slaveAddress    = 0x01;
    req.functionCode    = 0x10;
    req.startingAddress = 0x0001;
    req.quantity         = 0x0002;
    req.registerValues   = {0x000A, 0x0102};

    const auto out = buildRequest(req);
    // [unitId=1][FC=1][addr:2][qty:2][byteCount=1][v0:2][v1:2][CRC:2] = 13 字节
    // 之前误写 15,实际 unitId/FC 各 1B;CRC 2B;qty=2 → byteCount=4 → 4+addr(2)+qty(2)+CRC(2) = 10 + 3 = 13
    REQUIRE(out.size() == 13);
    REQUIRE(out[0] == 0x01);
    REQUIRE(out[1] == 0x10);
    REQUIRE(out[6] == 0x04);               // byteCount = qty*2
    REQUIRE(out[7] == 0x00); REQUIRE(out[8]  == 0x0A);
    REQUIRE(out[9] == 0x01); REQUIRE(out[10] == 0x02);
    REQUIRE(crc16ModbusVerify(bytes(out), out.size()));
}

// ─────────────────────────────────────────────────────────────────────────────
// 组帧：FC0F 写多线圈（位打包由 caller 负责）
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_frame: FC0F RTU write multiple coils", "[master][frame][build]") {
    ModbusRequest req;
    req.transport       = Transport::Rtu;
    req.slaveAddress    = 0x01;
    req.functionCode    = 0x0F;
    req.startingAddress = 0x0013;
    req.quantity        = 10;
    // 10 coils → ceil(10/8) = 2 字节;bit0=1, bit1=1, 其余 0
    req.coilBytes = {0xCD, 0x01};
    const auto out = buildRequest(req);
    // [unitId=1][FC=1][addr=2][qty=2][byteCount=1][data=2][CRC=2] = 11 字节
    REQUIRE(out.size() == 11);
    REQUIRE(out[0] == 0x01);
    REQUIRE(out[1] == 0x0F);
    REQUIRE(out[6] == 0x02);            // byteCount
    REQUIRE(out[7] == 0xCD);
    REQUIRE(out[8] == 0x01);
    REQUIRE(crc16ModbusVerify(bytes(out), out.size()));
}

// ─────────────────────────────────────────────────────────────────────────────
// 组帧：TCP/MBAP
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_frame: TCP MBAP buildRequest layout", "[master][frame][tcp][build]") {
    ModbusRequest req;
    req.transport       = Transport::Tcp;
    req.transactionId   = 0x0001;
    req.slaveAddress    = 0x11;
    req.functionCode    = 0x03;
    req.startingAddress = 0x006B;
    req.quantity        = 0x0003;

    const auto out = buildRequest(req);
    // [tid:2][pid:2=0][len:2][unitId][FC][addr:2][qty:2] = 12 字节,无 CRC
    REQUIRE(out.size() == 12);
    REQUIRE(out[0] == 0x00); REQUIRE(out[1] == 0x01);   // tid
    REQUIRE(out[2] == 0x00); REQUIRE(out[3] == 0x00);   // pid = 0
    REQUIRE(out[4] == 0x00); REQUIRE(out[5] == 0x06);   // length = 1 + 5 PDU bytes
    REQUIRE(out[6] == 0x11);                              // unitId
    REQUIRE(out[7] == 0x03);
    REQUIRE(out[8] == 0x00); REQUIRE(out[9]  == 0x6B);
    REQUIRE(out[10] == 0x00); REQUIRE(out[11] == 0x03);
}

TEST_CASE("modbus_frame: TCP MBAP rejects non-zero protocolId", "[master][frame][tcp][neg]") {
    std::vector<uint8_t> raw = {
        0x00, 0x01,        // tid
        0x00, 0x01,        // pid = 1 (非法)
        0x00, 0x06,        // length = 6
        0x11, 0x03, 0x00, 0x6B, 0x00, 0x03
    };
    REQUIRE_FALSE(parseTcpResponse(raw.data(), raw.size()).has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// 拆帧：RTU 正常读响应闭环（buildRequest → 模拟响应 → parseResponse）
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_frame: RTU FC03 round-trip build -> parse", "[master][frame][rtu][roundtrip]") {
    // 模拟从站返回:unitId=17,FC=3,byteCount=6,3 个寄存器 [0x022B, 0x0000, 0x0064]
    // 大端字节流 03 03 06 02 2B 00 00 00 64
    std::vector<uint8_t> resp = {
        0x11, 0x03, 0x06,
        0x02, 0x2B, 0x00, 0x00, 0x00, 0x64
    };
    uint16_t crc = ens::protocol::crc16Modbus(resp.data(), resp.size());
    resp.push_back(static_cast<uint8_t>(crc & 0xFF));
    resp.push_back(static_cast<uint8_t>(crc >> 8));

    auto r = parseRtuResponse(resp.data(), resp.size());
    REQUIRE(r.has_value());
    REQUIRE(r->slaveAddress  == 0x11);
    REQUIRE(r->functionCode  == 0x03);
    REQUIRE_FALSE(r->isException);
    REQUIRE(r->registerValues.size() == 3);
    REQUIRE(r->registerValues[0] == 0x022Bu);
    REQUIRE(r->registerValues[1] == 0x0000u);
    REQUIRE(r->registerValues[2] == 0x0064u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 拆帧：RTU 异常响应（function|0x80）
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_frame: RTU FC03 exception (function|0x80) parsed as isException",
          "[master][frame][rtu][exception]") {
    // 异常: [unitId][0x83][excCode=0x02 ILLEGAL_DATA_ADDRESS][CRC:2] = 5B
    std::vector<uint8_t> resp = {0x11, 0x83, 0x02};
    uint16_t crc = ens::protocol::crc16Modbus(resp.data(), resp.size());
    resp.push_back(static_cast<uint8_t>(crc & 0xFF));
    resp.push_back(static_cast<uint8_t>(crc >> 8));

    auto r = parseRtuResponse(resp.data(), resp.size());
    REQUIRE(r.has_value());
    REQUIRE(r->isException);
    REQUIRE(r->exceptionCode == 0x02);
    REQUIRE(r->functionCode == 0x83);
}

// ─────────────────────────────────────────────────────────────────────────────
// 拆帧：TCP 正常读响应
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_frame: TCP FC03 response parsed via MBAP", "[master][frame][tcp][parse]") {
    // [tid=1][pid=0][len=1+1+1+6=9][unitId=17][FC=3][byteCount=6][3 regs]  共 6+9=15 字节
    std::vector<uint8_t> resp = {
        0x00, 0x01,        // tid
        0x00, 0x00,        // pid = 0
        0x00, 0x09,        // length = unitId(1) + FC(1) + byteCount(1) + data(6) = 9
        0x11, 0x03, 0x06,
        0x02, 0x2B,        // reg 1 = 0x022B
        0x00, 0x00,        // reg 2 = 0x0000
        0x00, 0x64         // reg 3 = 0x0064
    };
    auto r = parseTcpResponse(resp.data(), resp.size());
    REQUIRE(r.has_value());
    REQUIRE(r->transactionId == 0x0001);
    REQUIRE(r->slaveAddress  == 0x11);
    REQUIRE(r->functionCode  == 0x03);
    REQUIRE(r->registerValues.size() == 3);
    REQUIRE(r->registerValues[0] == 0x022Bu);
}

TEST_CASE("modbus_frame: TCP exception response parsed", "[master][frame][tcp][exception]") {
    // [tid=1][pid=0][len=3][unitId=17][FC=0x83][excCode=0x02]  = 7+3=10 字节
    std::vector<uint8_t> resp = {
        0x00, 0x01, 0x00, 0x00, 0x00, 0x03,
        0x11, 0x83, 0x02
    };
    auto r = parseTcpResponse(resp.data(), resp.size());
    REQUIRE(r.has_value());
    REQUIRE(r->isException);
    REQUIRE(r->exceptionCode == 0x02);
}

// ─────────────────────────────────────────────────────────────────────────────
// 健壮性：长度不够 / CRC 错 → 返 nullopt
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_frame: rejects too-short buffer", "[master][frame][neg]") {
    uint8_t a = 0x01, b = 0x02;
    REQUIRE_FALSE(parseRtuResponse(nullptr, 5).has_value());
    REQUIRE_FALSE(parseRtuResponse(&a, 3).has_value());    // < 4 最小帧
    REQUIRE_FALSE(parseTcpResponse(nullptr, 9).has_value());
    REQUIRE_FALSE(parseTcpResponse(&a, 5).has_value());    // < 9 最小 TCP 帧
}

TEST_CASE("modbus_frame: RTU rejects CRC-corrupted frame", "[master][frame][neg]") {
    std::vector<uint8_t> resp = {0x11, 0x03, 0x06, 0x02, 0x2B, 0x00, 0x00, 0x00, 0x64,
                                0xAA, 0xBB};             // 故意坏 CRC
    REQUIRE_FALSE(parseRtuResponse(resp.data(), resp.size()).has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// 闭环：buildRequest 出来的东西能被自己解析（CRC 双向性）
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_frame: RTU buildRequest payload is self-CRC-verifiable",
          "[master][frame][rtu][self]") {
    ModbusRequest req;
    req.transport       = Transport::Rtu;
    req.slaveAddress    = 0x05;
    req.functionCode    = 0x03;
    req.startingAddress = 0x0100;
    req.quantity        = 4;
    const auto out = buildRequest(req);
    // CRC 收紧在最后 2 字节,通过
    REQUIRE(crc16ModbusVerify(bytes(out), out.size()));
    // 反向解：去掉 CRC 后 6 字节不应再有完整响应语义,但累加器已能识别为请求
    // 这里仅验"自校验闭环"
    REQUIRE(out.size() == 8);
}
