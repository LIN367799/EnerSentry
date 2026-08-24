// core/mbap.h —— 测试台公共契约：Modbus TCP 报文头 MBAP（ENS-SIM-IMP §2.2）。
// MBAP 头 7 字节：transactionId(2, 大端) + protocolId(2, 大端, 固定 0x0000)
//                + length(2, 大端, = Unit ID + PDU 长度, 不含 MBAP 头本身) + unitId(1)。
// 纯 C++17，零依赖。
#pragma once

#include <cstddef>
#include <cstdint>

namespace ens::core {

struct MbapHeader {
    uint16_t transactionId;   // 请求/响应配对标识（原样透传，不参与计算）
    uint16_t protocolId = 0;  // 固定 0x0000
    uint16_t length;          // Unit ID + PDU 长度（不含本头 7 字节）
    uint8_t  unitId;          // 从站地址（Unit ID）
};

// 解析 p[0..n)，成功（n>=7 且 protocolId==0）则填 out 并返回 true；否则 false 且 out 不变。
// 所有 16-bit 字段按网络字节序（大端）解出。
bool parse_mbap(const uint8_t* p, size_t n, MbapHeader& out) noexcept;

// 把 h 序列化到 p[0..7)（大端）；length 由调用方先算好填进 h。
void emit_mbap(uint8_t* p, const MbapHeader& h) noexcept;

}  // namespace ens::core