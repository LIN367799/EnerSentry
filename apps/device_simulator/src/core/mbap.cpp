// core/mbap.cpp —— MBAP 头解析/序列化（ENS-SIM-IMP §2.2）。
// 大端字节序：所有 16-bit 字段高位在前。length 语义 = unitId + PDU 字节数（不含 MBAP 头）。
#include "core/mbap.h"

namespace ens::core {

bool parse_mbap(const uint8_t* p, size_t n, MbapHeader& out) noexcept {
    if (n < 7) return false;
    const uint16_t tid = static_cast<uint16_t>((p[0] << 8) | p[1]);
    const uint16_t pid = static_cast<uint16_t>((p[2] << 8) | p[3]);
    if (pid != 0) return false;                       // Modbus/TCP 强制 protocolId=0x0000
    const uint16_t len = static_cast<uint16_t>((p[4] << 8) | p[5]);
    out.transactionId = tid;
    out.protocolId    = pid;
    out.length        = len;
    out.unitId        = p[6];
    return true;
}

void emit_mbap(uint8_t* p, const MbapHeader& h) noexcept {
    p[0] = static_cast<uint8_t>(h.transactionId >> 8);
    p[1] = static_cast<uint8_t>(h.transactionId & 0xFF);
    p[2] = static_cast<uint8_t>(h.protocolId >> 8);
    p[3] = static_cast<uint8_t>(h.protocolId & 0xFF);
    p[4] = static_cast<uint8_t>(h.length >> 8);
    p[5] = static_cast<uint8_t>(h.length & 0xFF);
    p[6] = h.unitId;
}

}  // namespace ens::core