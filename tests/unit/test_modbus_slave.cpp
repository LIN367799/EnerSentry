// tests/unit/test_modbus_slave.cpp
// B3 从站请求分派 Tier 2 单测（ENS-DEV-GUIDE §2B B3：buildReadResponse 字节序列正确）。
// 覆盖：FC03/04 读大端序列 / 越界 0x02 / 非法数量 0x03 / FC06 写回显+落盘 / 未知功能码 0x01。

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include "sim/modbus_slave.h"

using ens::sim::SlaveRegs;
using ens::sim::dispatchRequest;

static std::vector<uint8_t> readReq(uint8_t fc, uint16_t addr, uint16_t qty) {
    return {fc, static_cast<uint8_t>(addr >> 8), static_cast<uint8_t>(addr & 0xFF),
            static_cast<uint8_t>(qty >> 8), static_cast<uint8_t>(qty & 0xFF)};
}

TEST_CASE("slave: FC03 read returns big-endian register values", "[sim][slave]") {
    SlaveRegs regs;
    regs.set(0, 0x1234);
    regs.set(1, 0xABCD);

    const auto pdu = readReq(0x03, 0, 2);
    const auto resp = dispatchRequest(regs, pdu.data(), pdu.size());

    REQUIRE(resp.size() == 1 + 1 + 4);
    CHECK(resp[0] == 0x03);                 // 功能码回显
    CHECK(resp[1] == 4);                    // byteCount = 2 regs × 2B
    CHECK(resp[2] == 0x12);                 // 大端：0x1234 → 0x12 0x34
    CHECK(resp[3] == 0x34);
    CHECK(resp[4] == 0xAB);
    CHECK(resp[5] == 0xCD);
}

TEST_CASE("slave: FC03 read beyond 64 registers -> exception 0x02", "[sim][slave]") {
    SlaveRegs regs;
    const auto pdu = readReq(0x03, 60, 8);   // 60+8 > 64
    const auto resp = dispatchRequest(regs, pdu.data(), pdu.size());

    REQUIRE(resp.size() == 2);
    CHECK(resp[0] == 0x83);                 // fc | 0x80
    CHECK(resp[1] == 0x02);                 // ILLEGAL DATA ADDRESS
}

TEST_CASE("slave: FC03 zero quantity -> exception 0x03", "[sim][slave]") {
    SlaveRegs regs;
    const auto pdu = readReq(0x03, 0, 0);
    const auto resp = dispatchRequest(regs, pdu.data(), pdu.size());

    REQUIRE(resp.size() == 2);
    CHECK(resp[0] == 0x83);
    CHECK(resp[1] == 0x03);                 // ILLEGAL DATA VALUE
}

TEST_CASE("slave: FC06 write echoes and persists value", "[sim][slave]") {
    SlaveRegs regs;
    const std::vector<uint8_t> pdu = {0x06, 0x00, 0x03, 0x12, 0x34};   // 写 addr=3 val=0x1234
    const auto resp = dispatchRequest(regs, pdu.data(), pdu.size());

    REQUIRE(resp == pdu);                   // 写回显：原样返回
    CHECK(regs.get(3) == 0x1234);           // 已落盘

    // 读回验证
    const auto rd = dispatchRequest(regs, readReq(0x03, 3, 1).data(), 5);
    REQUIRE(rd.size() == 4);
    CHECK(rd[2] == 0x12);
    CHECK(rd[3] == 0x34);
}

TEST_CASE("slave: FC06 write out of range -> exception 0x02", "[sim][slave]") {
    SlaveRegs regs;
    const std::vector<uint8_t> pdu = {0x06, 0x00, 64, 0x00, 0x01};     // addr=64 越界
    const auto resp = dispatchRequest(regs, pdu.data(), pdu.size());

    REQUIRE(resp.size() == 2);
    CHECK(resp[0] == 0x86);
    CHECK(resp[1] == 0x02);
}

TEST_CASE("slave: unknown function code -> exception 0x01", "[sim][slave]") {
    SlaveRegs regs;
    const std::vector<uint8_t> pdu = {0x7F, 0x00, 0x00, 0x00, 0x01};
    const auto resp = dispatchRequest(regs, pdu.data(), pdu.size());

    REQUIRE(resp.size() == 2);
    CHECK(resp[0] == 0xFF);                 // 0x7F | 0x80
    CHECK(resp[1] == 0x01);                 // ILLEGAL FUNCTION
}

TEST_CASE("slave: empty PDU yields empty response (transport drops)", "[sim][slave]") {
    SlaveRegs regs;
    const auto resp = dispatchRequest(regs, nullptr, 0);
    CHECK(resp.empty());
}