// src/protocol/ModbusFrame.cpp
// L2 协议引擎 ── 组帧/拆帧实现（ENS-LLD-100 §4.1.1 / §4.1.3 / §4.2.2）。
//
// 字节契约：
//   * 所有 16-bit 字段按网络序（大端）。
//   * CRC: RTU 末 2 字节低字节在前（crc&0xFF, crc>>8）。
//   * MBAP: [tid:2][pid:2][len:2][unitId:1][PDU...]；len = unitId(1) + PDU 字节数。
//
// 函数覆盖矩阵：
//   ┌──────────┬─────────────────────────────────────────┐
//   │ FC01/02  │ 读线圈/离散输入                       │
//   │ FC03/04  │ 读保持/输入寄存器                     │
//   │ FC05/06  │ 写单线圈/单寄存器                     │
//   │ FC0F/10  │ 写多线圈/多寄存器                     │
//   │ function │ (function & 0x80) → 异常帧定长 5/9 字节 │
//   └──────────┴─────────────────────────────────────────┘
//
// 设计要点：
//   * 异常帧一律走 isException=true 分支,不与正常响应混解析（避免 byteCount 错位）。
//   * 解析输入约定为"已被 ModbusStreamAccumulator 切好的完整帧"，
//     长度不足返 std::nullopt,CRC 错返 std::nullopt,不抛异常。
//   * 错误模式统一由返回 std::nullopt 表示,调用方(3.1.3 ModbusEngine)发 frameError 信号。

#include "ModbusFrame.h"

#include <cstring>

namespace ens::protocol {

// ── 内部辅助：写 16-bit 大端,写 8-bit,均 noexcept ─────────────────────────
namespace {

inline void writeU16BE(std::vector<uint8_t>& v, uint16_t x) noexcept {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}

inline uint16_t readU16BE(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(p[0]) << 8) |
         static_cast<uint16_t>(p[1]));
}

// FC0F 线圈位打包：由 caller 提供 bit-packed 字节流（这里直接附加,不做位运算）。
// 调用方 ModbusEngine(3.1.3)负责位打包细节,本层只搬运字节。
// FC10 寄存器列表按大端复制两个字节/项。

// MBAP 头（7 字节,tid+pid+len+unitId）。unitId 由 caller 直接传入真实值，
// 不在本函数内写 placeholder,避免上层误以为还要再 push 一遍 unitId。
// pduLenPlusUnit = mbap 头之后字节数（即 unitId 字节数 + caller 后续 push 的 PDU 字节数），
// 也就是 MBAP "Length" 字段的语义（含 unitId）。
inline void appendMbap(std::vector<uint8_t>& v, uint16_t tid,
                       uint8_t unitId, uint16_t pduLenPlusUnit) noexcept {
    writeU16BE(v, tid);
    writeU16BE(v, 0x0000);                // protocolId = 0 (Modbus/TCP 强制)
    writeU16BE(v, pduLenPlusUnit);        // length = unitId + PDU bytes
    v.push_back(unitId);
}

}  // namespace

// ── 组帧 ─────────────────────────────────────────────────────────────────────

std::vector<uint8_t> buildRequest(const ModbusRequest& req) noexcept {
    std::vector<uint8_t> out;
    if (req.slaveAddress < 1 || req.slaveAddress > 247) return out;  // 保留地址

    // ── PDU 构造（不含 unitId/MBAP/CRC,后续按 transport 段位追加）──
    std::vector<uint8_t> pdu;
    switch (req.functionCode) {
        case 0x01: case 0x02: case 0x03: case 0x04: {
            // 读类请求：[FC][addr:2][qty:2] 共 5 字节 PDU
            if (req.quantity == 0) return out;       // 显式拒绝空读（量化承诺 NFR-PERF-04）
            pdu.push_back(req.functionCode);
            writeU16BE(pdu, req.startingAddress);
            writeU16BE(pdu, req.quantity);
            break;
        }
        case 0x05: {
            // 写单线圈：[FC][addr:2][value:2] 共 5 字节；value 仅 0x0000 / 0xFF00
            if (req.registerValues.size() != 1) return out;
            const uint16_t v = req.registerValues[0];
            if (v != 0x0000 && v != 0xFF00) return out;
            pdu.push_back(req.functionCode);
            writeU16BE(pdu, req.startingAddress);
            writeU16BE(pdu, v);
            break;
        }
        case 0x06: {
            // 写单寄存器：[FC][addr:2][value:2] 共 5 字节
            if (req.registerValues.size() != 1) return out;
            pdu.push_back(req.functionCode);
            writeU16BE(pdu, req.startingAddress);
            writeU16BE(pdu, req.registerValues[0]);
            break;
        }
        case 0x0F: {
            // 写多线圈：[FC][addr:2][qty:2][byteCount][coilBytes...] 共 5+1+N 字节
            if (req.quantity == 0 || req.coilBytes.empty()) return out;
            const size_t expectedBytes = (req.quantity + 7) / 8;
            if (req.coilBytes.size() != expectedBytes) return out;
            pdu.push_back(req.functionCode);
            writeU16BE(pdu, req.startingAddress);
            writeU16BE(pdu, req.quantity);
            pdu.push_back(static_cast<uint8_t>(expectedBytes));
            pdu.insert(pdu.end(), req.coilBytes.begin(), req.coilBytes.end());
            break;
        }
        case 0x10: {
            // 写多寄存器：[FC][addr:2][qty:2][byteCount=N*2][data:N*2B] 共 9+N*2 字节
            if (req.quantity == 0 || req.registerValues.empty()) return out;
            if (req.registerValues.size() != req.quantity) return out;
            pdu.push_back(req.functionCode);
            writeU16BE(pdu, req.startingAddress);
            writeU16BE(pdu, req.quantity);
            pdu.push_back(static_cast<uint8_t>(req.quantity * 2));
            for (uint16_t v : req.registerValues) writeU16BE(pdu, v);
            break;
        }
        default:
            return out;     // 不支持的功能码 → 空 vector,由 Engine 计 frameError
    }

    if (req.transport == Transport::Tcp) {
        // ── TCP：MBAP 7 字节（tid+pid+len+unitId）+ caller 后续 push PDU(无 CRC) ──
        // mbap "Length" = unitId(1) + caller-pushed PDU 字节数 = 1 + pdu.size()
        const uint16_t pduLenPlusUnit = static_cast<uint16_t>(1 + pdu.size());
        appendMbap(out, req.transactionId, req.slaveAddress, pduLenPlusUnit);
        out.insert(out.end(), pdu.begin(), pdu.end());
    } else {
        // ── RTU：unitId + PDU + CRC(低字节在前) ──
        out.push_back(req.slaveAddress);
        out.insert(out.end(), pdu.begin(), pdu.end());
        const uint16_t crc = crc16Modbus(out.data(), out.size());
        out.push_back(static_cast<uint8_t>(crc & 0xFF));
        out.push_back(static_cast<uint8_t>(crc >> 8));
    }
    return out;
}

// ── 拆帧：TCP ──────────────────────────────────────────────────────────────

std::optional<ModbusResponse> parseTcpResponse(const uint8_t* buf, size_t len) noexcept {
    // MBAP 头固定 6 字节 (tid+pid+len) + 至少后续 2 字节 (unitId+FC) → len >= 8
    if (buf == nullptr || len < 8) return std::nullopt;

    const uint16_t tid = readU16BE(buf + 0);
    const uint16_t pid = readU16BE(buf + 2);
    if (pid != 0x0000) return std::nullopt;
    const uint16_t mbapLen = readU16BE(buf + 4);
    // MBAP "Length" 字段语义 = 自 Length 字段之后（含 unitId）的所有字节数。
    // 所以总帧长 = 6 (tid+pid+len) + mbapLen。
    // mbapLen >= 2 (unitId + 至少 1 字节 FC)。
    if (mbapLen < 2 || 6u + mbapLen > len) return std::nullopt;

    ModbusResponse resp(Transport::Tcp, /*unitId=*/buf[6], /*fc=*/buf[7]);
    resp.transactionId = tid;

    // 异常帧：[unitId][0x80|FC][excCode] = mbapLen - 1 字节 PDU（即 mbapLen == 3）
    if (resp.functionCode & 0x80) {
        if (mbapLen != 3) return std::nullopt;
        resp.isException   = true;
        resp.exceptionCode = buf[8];
        return resp;
    }

    // 正常帧处理（与 RTU 共享解析逻辑,但跳过 CRC 校验）
    switch (resp.functionCode) {
        case 0x01: case 0x02: case 0x03: case 0x04: {
            // [unitId][FC][byteCount][data...];mbapLen = 3 + byteCount
            if (mbapLen < 3) return std::nullopt;
            const uint8_t byteCount = buf[8];
            if (static_cast<size_t>(3 + byteCount) != mbapLen) return std::nullopt;
            // 数据区起点 = buf[9]，长度 = byteCount（已被 mbapLen 门禁兜底，此处不必再检查）
            if (resp.functionCode == 0x01 || resp.functionCode == 0x02) {
                resp.coilBytes.assign(buf + 9, buf + 9 + byteCount);
            } else {
                if (byteCount % 2 != 0) return std::nullopt;
                for (size_t i = 0; i < byteCount; i += 2)
                    resp.registerValues.push_back(readU16BE(buf + 9 + i));
            }
            return resp;
        }
        case 0x05: case 0x06: {
            // [unitId][FC][addr:2][echo:2];mbapLen = 6
            if (mbapLen != 6) return std::nullopt;
            resp.registerValues.push_back(readU16BE(buf + 8));
            return resp;
        }
        case 0x0F: {
            // [unitId][FC][addr:2][qty:2];mbapLen = 6
            if (mbapLen != 6) return std::nullopt;
            return resp;
        }
        case 0x10: {
            // [unitId][FC][addr:2][qty:2];mbapLen = 6
            if (mbapLen != 6) return std::nullopt;
            return resp;
        }
        default:
            return std::nullopt;
    }
}

// ── 拆帧：RTU ──────────────────────────────────────────────────────────────

std::optional<ModbusResponse> parseRtuResponse(const uint8_t* buf, size_t len) noexcept {
    // RTU 最小帧：unitId+FC+CRC = 4 字节
    if (buf == nullptr || len < 4) return std::nullopt;
    if (!crc16ModbusVerify(buf, len)) return std::nullopt;

    ModbusResponse resp(Transport::Rtu, /*unitId=*/buf[0], /*fc=*/buf[1]);

    // 异常帧：[unitId][0x80|FC][excCode][CRC:2] = 5 字节
    if (resp.functionCode & 0x80) {
        if (len != 5) return std::nullopt;
        resp.isException   = true;
        resp.exceptionCode = buf[2];
        return resp;
    }

    switch (resp.functionCode) {
        case 0x01: case 0x02: case 0x03: case 0x04: {
            if (len < 5) return std::nullopt;
            const uint8_t byteCount = buf[2];
            if (static_cast<size_t>(5 + byteCount) != len) return std::nullopt;
            if (resp.functionCode == 0x01 || resp.functionCode == 0x02) {
                resp.coilBytes.assign(buf + 3, buf + 3 + byteCount);
            } else {
                if (byteCount % 2 != 0) return std::nullopt;
                for (size_t i = 0; i < byteCount; i += 2)
                    resp.registerValues.push_back(readU16BE(buf + 3 + i));
            }
            return resp;
        }
        case 0x05: case 0x06: case 0x0F: case 0x10: {
            // 写类回显：[unitId][FC][addr:2][qty_or_value:2][CRC:2] = 8 字节
            if (len != 8) return std::nullopt;
            if (resp.functionCode == 0x05 || resp.functionCode == 0x06) {
                resp.registerValues.push_back(readU16BE(buf + 4));
            }
            return resp;
        }
        default:
            return std::nullopt;
    }
}

// ── 拆帧：通用入口（按 buf[0]/buf[1] 无法判别 RTU/TCP,故强制 caller 指明）──

std::optional<ModbusResponse> parseResponse(const uint8_t* buf, size_t len) noexcept {
    if (buf == nullptr || len < 4) return std::nullopt;
    // 启发式判定：前 2 字节全 0 几乎不会出现在 RTU（unitId != 0）,TCP mbap 以 tid(pid==0)
    // 开头。保守策略：先尝试 TCP,失败再尝试 RTU（调用方更推荐显式 parseTcp/parseRtu）。
    if (auto r = parseTcpResponse(buf, len)) return r;
    return parseRtuResponse(buf, len);
}

// ── 诊断 ─────────────────────────────────────────────────────────────────────

size_t expectedResponseBodySize(uint8_t functionCode, uint16_t quantity) noexcept {
    // 不含 CRC/MBAP,仅作为累加器"还差多少字节"提示;异常帧长度独立由 isException 路径判定。
    switch (functionCode) {
        case 0x01: case 0x02: {
            const size_t bytes = (quantity + 7) / 8;
            return 3 + bytes;       // [unitId][FC][byteCount][data...]
        }
        case 0x03: case 0x04: {
            return static_cast<size_t>(3 + 2 * quantity);
        }
        case 0x05: case 0x06: case 0x0F: case 0x10: {
            return 6;   // [unitId][FC][addr:2][qty/value:2] (no CRC)
        }
        default:
            return 0;
    }
}

}  // namespace ens::protocol
