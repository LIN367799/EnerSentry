// src/protocol/ModbusStreamAccumulator.cpp
// tryExtractFrame 帧边界状态机（ENS-LLD-100 §4.2.1.1）：
//   等待最小帧头 → 异常帧按 5B/9B 定长提取 → RTU 按功能码算长 + CRC 校验
//   → TCP 按 MBAP Length 精确切分 → 长度不定/CRC 失败 popFront(1) 重新同步。

#include "ModbusStreamAccumulator.h"

namespace ens::protocol {

bool ModbusStreamAccumulator::crcValid(size_t len) const noexcept {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len - 2; ++i) {
        crc ^= peek(i);
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x0001)
                ? static_cast<uint16_t>((crc >> 1) ^ 0xA001)   // poly 0xA001（反射）
                : static_cast<uint16_t>(crc >> 1);
    }
    const uint16_t crcWire = static_cast<uint16_t>(peek(len - 2))
                           | (static_cast<uint16_t>(peek(len - 1)) << 8);
    return crc == crcWire;
}

bool ModbusStreamAccumulator::tryExtractFrame(uint8_t* out, size_t& outLen, bool isTcp) noexcept {
    const size_t minHdr = isTcp ? 7u : 4u;          // TCP MBAP=7；RTU addr+func+2CRC=4
    if (m_size < minHdr) return false;

    // UnitID 偏移：TCP 在 MBAP 第 6 字节，RTU 在 0（UnitID 由上层 parseResponse 路由）。
    const size_t unitOff = isTcp ? 6u : 0u;
    const uint8_t function = peek(unitOff + 1);

    // 异常响应帧：功能码最高位置 1，固定 5B(RTU)/9B(TCP) 定长提取（V1.4）。
    if (function & 0x80) {
        const size_t expected = isTcp ? 9u : 5u;
        if (m_size < expected) return false;
        if (!isTcp && !crcValid(expected)) {          // RTU 异常帧也校验 CRC
            ++m_huntCount;
            popFront(1);
            return false;
        }
        pop(expected, out, outLen);
        onFrameExtracted();
        return true;
    }

    // 正常响应帧：按功能码 / MBAP Length 计算期望长度。
    size_t expected;
    if (isTcp) {
        // Length = UnitId + PDU 字节数 → 整帧 = 6 + Length；合法 Length 最小 2（UnitId+PDU≥1）。
        const uint16_t lenField = static_cast<uint16_t>(peek(4)) << 8 | peek(5);
        if (lenField < 2 || 6 + lenField > kCapacity) {   // 非法/超长（防死等）→ 重新同步
            popFront(1);
            return false;
        }
        expected = 6 + lenField;
    } else {
        const uint8_t byteCount = peek(2);
        switch (function) {
            case 0x01: case 0x02: case 0x03: case 0x04:  // 读类：addr+func+byteCount+data+CRC
                expected = 3 + byteCount + 2;
                break;
            case 0x05: case 0x06: case 0x0F: case 0x10:  // 写类：原样回显 8 字节
                expected = 8;
                break;
            default:                                     // 未知功能码，长度不定 → 前滑
                popFront(1);
                return false;
        }
    }

    if (m_size < expected) return false;                 // 帧未到齐，等待

    if (!isTcp && !crcValid(expected)) {                 // RTU CRC 失败 → 前滑
        ++m_huntCount;
        popFront(1);
        return false;
    }

    pop(expected, out, outLen);
    onFrameExtracted();
    return true;
}

}  // namespace ens::protocol
