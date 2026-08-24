// tests/unit/test_mbap.cpp
// ens::core MBAP 头解析/序列化 Tier 2 单测（ENS-DEV-GUIDE §2B B1）。
// 覆盖：emit 大端字节序 / parse 恢复与回环 / n<7 边界拒绝 / protocolId!=0 拒绝。

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

#include "core/mbap.h"

using ens::core::MbapHeader;
using ens::core::parse_mbap;
using ens::core::emit_mbap;

TEST_CASE("mbap: emit writes big-endian wire layout", "[core][mbap]") {
    MbapHeader h;
    h.transactionId = 0x1234;
    h.protocolId    = 0x0000;
    h.length        = 0x0105;   // 261 字节（unitId + PDU）
    h.unitId        = 0x0F;

    std::array<uint8_t, 7> wire{};
    emit_mbap(wire.data(), h);

    CHECK(wire[0] == 0x12);   // transactionId 高字节在前
    CHECK(wire[1] == 0x34);
    CHECK(wire[2] == 0x00);   // protocolId
    CHECK(wire[3] == 0x00);
    CHECK(wire[4] == 0x01);   // length 高字节在前
    CHECK(wire[5] == 0x05);
    CHECK(wire[6] == 0x0F);   // unitId 单字节
}

TEST_CASE("mbap: parse round-trips emit output", "[core][mbap]") {
    MbapHeader in;
    in.transactionId = 0xABCD;
    in.length        = 8;
    in.unitId        = 0x02;

    std::array<uint8_t, 7> wire{};
    emit_mbap(wire.data(), in);

    MbapHeader out{};
    REQUIRE(parse_mbap(wire.data(), wire.size(), out));
    CHECK(out.transactionId == 0xABCD);
    CHECK(out.protocolId == 0);
    CHECK(out.length == 8);
    CHECK(out.unitId == 0x02);
}

TEST_CASE("mbap: parse rejects buffer shorter than 7 bytes", "[core][mbap]") {
    const std::array<uint8_t, 6> shortBuf = {0, 1, 0, 0, 0, 6};
    MbapHeader out{};
    CHECK_FALSE(parse_mbap(shortBuf.data(), shortBuf.size(), out));
}

TEST_CASE("mbap: parse rejects non-zero protocolId", "[core][mbap]") {
    const std::array<uint8_t, 7> badPid = {0, 1, 0, 9, 0, 6, 3};  // protocolId=0x0009
    MbapHeader out{};
    CHECK_FALSE(parse_mbap(badPid.data(), badPid.size(), out));
}

TEST_CASE("mbap: parse leaves out untouched on failure", "[core][mbap]") {
    MbapHeader out;
    out.transactionId = 0xDEAD;
    out.length        = 0xBEEF;
    out.unitId        = 0x7F;

    const std::array<uint8_t, 3> tiny = {0, 0, 0};
    CHECK_FALSE(parse_mbap(tiny.data(), tiny.size(), out));
    CHECK(out.transactionId == 0xDEAD);
    CHECK(out.length == 0xBEEF);
    CHECK(out.unitId == 0x7F);
}