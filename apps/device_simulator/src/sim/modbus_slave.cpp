// sim/modbus_slave.cpp —— B3 最小从站实现（ENS-DEV-GUIDE §2B B3 / ENS-SIM-IMP §6.1）。
// 寄存器值按 Modbus 标准大端落线；FC06 写先落盘再回显（写回显语义，验证 SBO 控制链路）。
#include "sim/modbus_slave.h"

namespace ens::sim {

uint16_t SlaveRegs::get(size_t idx) const noexcept {
    std::lock_guard<std::mutex> lock(m_mtx);
    return (idx < kRegCount) ? m_regs[idx] : uint16_t{0};
}

bool SlaveRegs::set(size_t idx, uint16_t value) noexcept {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (idx >= kRegCount) return false;
    m_regs[idx] = value;
    return true;
}

std::array<uint16_t, SlaveRegs::kRegCount> SlaveRegs::snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_regs;
}

namespace {

constexpr uint16_t be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

std::vector<uint8_t> exceptionPdu(uint8_t fc, uint8_t code) noexcept {
    return {static_cast<uint8_t>(fc | 0x80), code};
}

// 读路径纯函数：基于寄存器快照组 FC03/04 响应
std::vector<uint8_t> buildReadResponse(const std::array<uint16_t, SlaveRegs::kRegCount>& regs,
                                       uint8_t fc, uint16_t addr, uint16_t qty) noexcept {
    if (qty == 0 || qty > 125) return exceptionPdu(fc, 0x03);               // ILLEGAL DATA VALUE
    if (static_cast<uint32_t>(addr) + qty > SlaveRegs::kRegCount)
        return exceptionPdu(fc, 0x02);                                      // ILLEGAL DATA ADDRESS

    std::vector<uint8_t> out;
    out.reserve(2 + static_cast<size_t>(qty) * 2);
    out.push_back(fc);
    out.push_back(static_cast<uint8_t>(qty * 2));                           // byteCount
    for (uint16_t i = 0; i < qty; ++i) {
        const uint16_t v = regs[static_cast<size_t>(addr) + i];
        out.push_back(static_cast<uint8_t>(v >> 8));                        // 大端
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    return out;
}

}  // namespace

std::vector<uint8_t> dispatchRequest(SlaveRegs& regs, const uint8_t* pdu, size_t n) noexcept {
    if (n < 1 || pdu == nullptr) return {};                                 // 空 PDU → 传输层丢弃连接
    const uint8_t fc = pdu[0];

    switch (fc) {
    case 0x03:                                                              // 读 Holding Registers
    case 0x04: {                                                            // 读 Input Registers
        if (n != 5) return exceptionPdu(fc, 0x03);
        const uint16_t addr = be16(pdu + 1);
        const uint16_t qty  = be16(pdu + 3);
        return buildReadResponse(regs.snapshot(), fc, addr, qty);
    }
    case 0x06: {                                                            // 写单寄存器（回显）
        if (n != 5) return exceptionPdu(fc, 0x03);
        const uint16_t addr = be16(pdu + 1);
        const uint16_t val  = be16(pdu + 3);
        if (addr >= SlaveRegs::kRegCount) return exceptionPdu(fc, 0x02);    // 越界
        if (!regs.set(addr, val)) return exceptionPdu(fc, 0x02);
        // 写回显：原样返回请求 PDU（含 fc+addr+val）
        return {fc, pdu[1], pdu[2], pdu[3], pdu[4]};
    }
    default:
        return exceptionPdu(fc, 0x01);                                      // ILLEGAL FUNCTION
    }
}

}  // namespace ens::sim