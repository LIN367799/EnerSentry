// sim/modbus_slave.h —— B3 最小从站：寄存器体 + 请求分派（ENS-DEV-GUIDE §2B B3）。
// 纯 C++17 零 Qt。功能码：FC03/04 读（大端）、FC06 写单寄存器（回显）、
// 异常响应：0x01 ILLEGAL FUNCTION / 0x02 ILLEGAL DATA ADDRESS / 0x03 ILLEGAL DATA VALUE。
// PDU 均**不含** MBAP 头（MBAP 由传输层负责，见 modbus_tcp_server.h）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ens::sim {

// 64 寄存器从站存储（B3 最小版；Phase 2 以 RegisterBank RCU 替换）
class SlaveRegs {
public:
    static constexpr size_t kRegCount = 64;

    uint16_t get(size_t idx) const noexcept;
    bool set(size_t idx, uint16_t value) noexcept;          // 越界返回 false
    std::array<uint16_t, kRegCount> snapshot() const noexcept;

private:
    mutable std::mutex m_mtx;
    std::array<uint16_t, kRegCount> m_regs{};
};

// 处理一个请求 PDU（不含 MBAP）并落盘写，返回响应 PDU（不含 MBAP；异常时 fc|0x80）。
// 空入参/非法结构 → 返回空 vector（传输层丢弃该连接，防脏数据）。
std::vector<uint8_t> dispatchRequest(SlaveRegs& regs, const uint8_t* pdu, size_t n) noexcept;

}  // namespace ens::sim