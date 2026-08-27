// apps/device_simulator/src/sim/modbus_dispatch.cpp
// 见 modbus_dispatch.h — 完整 FC01/02/03/04/05/06/15/16 dispatch 实现。
//
// 寄存器模型:
//   * SlaveRegset::holding[addr] / input[addr]  : uint16_t 数组,容量 = regCount
//   * SlaveRegset::coils[byte]    / discretes[byte]: uint8_t 位打包,byte[addr/8].bit[addr%8]
//   * 读越界(addr+qty 超容量)返 0x02 ILLEGAL DATA ADDRESS;单寄存器越界同样 0x02(协议合规)

#include "modbus_dispatch.h"

#include <cstring>

namespace ens::sim {

namespace {

constexpr uint16_t be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline void appendU16BE(std::vector<uint8_t>& v, uint16_t x) noexcept {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}

// 字节序无关的 coil 读:读 bit[reg%8] in byte[reg/8]
bool readCoil(const std::vector<uint8_t>& bytes, uint16_t reg) noexcept {
    const size_t byteIdx = reg / 8;
    if (byteIdx >= bytes.size()) return false;
    return (bytes[byteIdx] >> (reg % 8)) & 0x01;
}

void writeCoil(std::vector<uint8_t>& bytes, uint16_t reg, bool v) noexcept {
    const size_t byteIdx = reg / 8;
    if (byteIdx >= bytes.size()) return;
    const uint8_t mask = static_cast<uint8_t>(1u << (reg % 8));
    if (v) bytes[byteIdx] |= mask;
    else   bytes[byteIdx] &= static_cast<uint8_t>(~mask);
}

DispatchResult exceptionPdu(uint8_t fc, uint8_t code) noexcept {
    DispatchResult r;
    r.bytes = {static_cast<uint8_t>(fc | 0x80), code};
    r.exception = code;
    return r;
}

}  // namespace

DispatchResult dispatchFull(const SlaveRuntime& slaveRuntime,
                            const uint8_t* pdu, size_t n,
                            bool writable) noexcept {
    if (pdu == nullptr || n < 1 || slaveRuntime.regs == nullptr) {
        return exceptionPdu(0xFF, 0x01);        // ILLEGAL FUNCTION
    }
    const uint8_t fc = pdu[0];

    switch (fc) {
        // ─── FC01 读线圈 ────────────────────────────────────
        case 0x01: {
            if (n != 5) return exceptionPdu(fc, 0x03);
            const uint16_t addr = be16(pdu + 1);
            const uint16_t qty  = be16(pdu + 3);
            if (qty == 0 || qty > 2000) return exceptionPdu(fc, 0x03);
            if (static_cast<uint32_t>(addr) + qty > slaveRuntime.regs->coils.size() * 8u)
                return exceptionPdu(fc, 0x02);
            DispatchResult r;
            r.bytes.push_back(fc);
            const uint8_t byteCount = static_cast<uint8_t>((qty + 7) / 8);
            r.bytes.push_back(byteCount);
            r.bytes.resize(2 + byteCount, 0);
            for (uint16_t i = 0; i < qty; ++i) {
                if (readCoil(slaveRuntime.regs->coils, addr + i)) {
                    r.bytes[2 + (i / 8)] |= static_cast<uint8_t>(1u << (i % 8));
                }
            }
            return r;
        }

        // ─── FC02 读离散输入 ────────────────────────────────────
        case 0x02: {
            if (n != 5) return exceptionPdu(fc, 0x03);
            const uint16_t addr = be16(pdu + 1);
            const uint16_t qty  = be16(pdu + 3);
            if (qty == 0 || qty > 2000) return exceptionPdu(fc, 0x03);
            if (static_cast<uint32_t>(addr) + qty > slaveRuntime.regs->discretes.size() * 8u)
                return exceptionPdu(fc, 0x02);
            DispatchResult r;
            r.bytes.push_back(fc);
            const uint8_t byteCount = static_cast<uint8_t>((qty + 7) / 8);
            r.bytes.push_back(byteCount);
            r.bytes.resize(2 + byteCount, 0);
            for (uint16_t i = 0; i < qty; ++i) {
                if (readCoil(slaveRuntime.regs->discretes, addr + i)) {
                    r.bytes[2 + (i / 8)] |= static_cast<uint8_t>(1u << (i % 8));
                }
            }
            return r;
        }

        // ─── FC03 读 Holding Registers ──────────────────────────
        case 0x03: {
            if (n != 5) return exceptionPdu(fc, 0x03);
            const uint16_t addr = be16(pdu + 1);
            const uint16_t qty  = be16(pdu + 3);
            if (qty == 0 || qty > 125) return exceptionPdu(fc, 0x03);
            if (static_cast<uint32_t>(addr) + qty > slaveRuntime.regs->holding.size())
                return exceptionPdu(fc, 0x02);
            DispatchResult r;
            r.bytes.push_back(fc);
            r.bytes.push_back(static_cast<uint8_t>(qty * 2));
            for (uint16_t i = 0; i < qty; ++i) {
                appendU16BE(r.bytes, slaveRuntime.regs->getHolding(addr + i));
            }
            return r;
        }

        // ─── FC04 读 Input Registers ────────────────────────────
        case 0x04: {
            if (n != 5) return exceptionPdu(fc, 0x03);
            const uint16_t addr = be16(pdu + 1);
            const uint16_t qty  = be16(pdu + 3);
            if (qty == 0 || qty > 125) return exceptionPdu(fc, 0x03);
            if (static_cast<uint32_t>(addr) + qty > slaveRuntime.regs->input.size())
                return exceptionPdu(fc, 0x02);
            DispatchResult r;
            r.bytes.push_back(fc);
            r.bytes.push_back(static_cast<uint8_t>(qty * 2));
            for (uint16_t i = 0; i < qty; ++i) {
                appendU16BE(r.bytes, slaveRuntime.regs->getInput(addr + i));
            }
            return r;
        }

        // ─── FC05 写单线圈 ──────────────────────────────────────
        case 0x05: {
            if (!writable) return exceptionPdu(fc, 0x01);
            if (n != 5) return exceptionPdu(fc, 0x03);
            const uint16_t addr = be16(pdu + 1);
            const uint16_t val  = be16(pdu + 3);
            // Modbus 规范:value 仅 0xFF00(ON)/0x0000(OFF),其他值 → ILLEGAL DATA VALUE
            if (val != 0xFF00u && val != 0x0000u) return exceptionPdu(fc, 0x03);
            if (addr >= slaveRuntime.regs->coils.size() * 8) return exceptionPdu(fc, 0x02);
            writeCoil(slaveRuntime.regs->coils, addr, val == 0xFF00u);
            // 回显原 PDU
            DispatchResult r;
            for (size_t i = 0; i < 5; ++i) r.bytes.push_back(pdu[i]);
            return r;
        }

        // ─── FC06 写单寄存器 ────────────────────────────────────
        case 0x06: {
            if (!writable) return exceptionPdu(fc, 0x01);
            if (n != 5) return exceptionPdu(fc, 0x03);
            const uint16_t addr = be16(pdu + 1);
            const uint16_t val  = be16(pdu + 3);
            if (addr >= slaveRuntime.regs->holding.size()) return exceptionPdu(fc, 0x02);
            slaveRuntime.regs->setHolding(addr, val);
            DispatchResult r;
            for (size_t i = 0; i < 5; ++i) r.bytes.push_back(pdu[i]);
            return r;
        }

        // ─── FC0F (15) 写多线圈 ─────────────────────────────────
        case 0x0F: {
            if (!writable) return exceptionPdu(fc, 0x01);
            if (n < 7) return exceptionPdu(fc, 0x03);
            const uint16_t addr = be16(pdu + 1);
            const uint16_t qty  = be16(pdu + 3);
            const uint8_t byteCount = pdu[5];
            if (qty == 0 || qty > 1968) return exceptionPdu(fc, 0x03);
            if (byteCount != (qty + 7) / 8) return exceptionPdu(fc, 0x03);
            if (6 + byteCount != n) return exceptionPdu(fc, 0x03);
            if (static_cast<uint32_t>(addr) + qty > slaveRuntime.regs->coils.size() * 8u)
                return exceptionPdu(fc, 0x02);
            for (uint16_t i = 0; i < qty; ++i) {
                const bool on = (pdu[6 + i / 8] >> (i % 8)) & 0x01;
                writeCoil(slaveRuntime.regs->coils, addr + i, on);
            }
            DispatchResult r;
            r.bytes.push_back(fc);
            appendU16BE(r.bytes, addr);
            appendU16BE(r.bytes, qty);
            return r;
        }

        // ─── FC10 (16) 写多寄存器 ───────────────────────────────
        case 0x10: {
            if (!writable) return exceptionPdu(fc, 0x01);
            if (n < 7) return exceptionPdu(fc, 0x03);
            const uint16_t addr = be16(pdu + 1);
            const uint16_t qty  = be16(pdu + 3);
            const uint8_t byteCount = pdu[5];
            if (qty == 0 || qty > 123) return exceptionPdu(fc, 0x03);
            if (byteCount != qty * 2)   return exceptionPdu(fc, 0x03);
            if (6 + byteCount != n)     return exceptionPdu(fc, 0x03);
            if (static_cast<uint32_t>(addr) + qty > slaveRuntime.regs->holding.size())
                return exceptionPdu(fc, 0x02);
            for (uint16_t i = 0; i < qty; ++i) {
                slaveRuntime.regs->setHolding(addr + i,
                    be16(pdu + 6 + 2 * i));
            }
            DispatchResult r;
            r.bytes.push_back(fc);
            appendU16BE(r.bytes, addr);
            appendU16BE(r.bytes, qty);
            return r;
        }

        default:
            return exceptionPdu(fc, 0x01);
    }
}

std::optional<DispatchResult> dispatchBySlaveId(
    uint8_t slaveId,
    RegisterBank& bank,
    const std::vector<SlaveRuntime>& allSlaves,
    const uint8_t* pdu, size_t n) noexcept {
    // 路由:在 allSlaves 中按 slaveId 找 metadata
    const SlaveRuntime* found = nullptr;
    for (const auto& s : allSlaves) {
        if (s.slaveId == slaveId) { found = &s; break; }
    }
    if (found == nullptr) return std::nullopt;
    auto snap = bank.snapshot(slaveId);
    if (snap == nullptr) return std::nullopt;

    const uint8_t fc = (pdu != nullptr && n >= 1) ? pdu[0] : 0;
    const bool isWrite = (fc == 0x05 || fc == 0x06 || fc == 0x0F || fc == 0x10);

    SlaveRuntime rt = *found;
    std::shared_ptr<SlaveRegset> writeCopy;    // 写路径 CoW 副本
    if (isWrite) {
        writeCopy = std::make_shared<SlaveRegset>(*snap);
        rt.regs = writeCopy.get();
    } else {
        // 读路径零拷贝:dispatchFull 对读 FC 仅调用 const 方法(get*/readCoil)
        rt.regs = const_cast<SlaveRegset*>(snap.get());
    }

    DispatchResult r = dispatchFull(rt, pdu, n, isWrite);
    if (isWrite && !r.exception.has_value() && writeCopy) {
        bank.publish(slaveId, std::shared_ptr<const SlaveRegset>(writeCopy));
    }
    return r;
}

}  // namespace ens::sim