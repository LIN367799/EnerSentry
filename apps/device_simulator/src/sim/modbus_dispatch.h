// apps/device_simulator/src/sim/modbus_dispatch.h
// Modbus FC 全功能码 dispatch ── Phase 1 Track B B6（ENS-LLD-SIM §4.1 / ENS-SIM-IMP §3.4）。
//
// 独立出 B3 内部的 dispatchRequest(B3 SlaveRegs&),升级为按 slaveId + SlaveRegset
// 的多从站路由版本；覆盖 NFR §3 完整功能码 FC01/02/03/04/05/06/15/16。
//
// 寄存器地址是从站作用域偏移(LLD-SIM §4.4)：addr 范围 [0, SlaveRegset::regCount)
// 对应从站私有寄存器;不同 slave 同样地址不冲突(如 BMS #1 的 0x1000 与 BMS #2
// 的 0x1000 走不同 SlaveRegset)。

#pragma once

#include <cstddef>
#include <cstdint>

#include <optional>
#include <vector>

#include "core/point_table.h"   // DeviceKind / Transport
#include "sim/register_bank.h"

namespace ens::sim {

// ─────────────────────────────────────────────────────────────────────────────
// SlaveRuntime —— SlaveRegset + SlaveSpec(transport/regCount 路由元数据)
// 不持有所有权:SlaveRegset 由 RegisterBank 拥有,emulator 拿非 const ptr
// （写路径 FC05/06/0F/10 需通过 bank.writeHolding/writeCoil 走 CoW）
// ─────────────────────────────────────────────────────────────────────────────
struct SlaveRuntime {
    uint8_t                slaveId = 0;
    DeviceKind             kind    = DeviceKind::Bms;
    Transport              transport = Transport::Tcp;
    SlaveRegset*           regs    = nullptr;     // pointer into RegisterBank snapshot
};

// ─────────────────────────────────────────────────────────────────────────────
// DispatchResult —— Modbus response builder 公共返回值
//   bytes: response PDU(不含 unitId / MBAP / CRC,运输层自己加)
//   exception: 若非 nullopt,ModbusFrame 把 FC|0x80 拼接进去(per LLD-100 §4.2.1)
// ─────────────────────────────────────────────────────────────────────────────
struct DispatchResult {
    std::vector<uint8_t> bytes;
    std::optional<uint8_t> exception;     // 0x01 ILLEGAL FUNCTION / 0x02 ILLEGAL DATA ADDR / 0x03 ILLEGAL DATA VALUE
};

// ─────────────────────────────────────────────────────────────────────────────
// dispatchFull —— 完整 FC01/02/03/04/05/06/15/16 dispatch
//   slaveRuntime: 从站路由元数据 + regs
//   pdu: FC + 数据(不含 unitId / CRC / MBAP 头)
//   writable: 写路径(FC05/06/0F/10)是否允许改 regs。读路径传 false(仅 const 访问),
//             写路径传 true(regs 必须指向 CoW 副本,调用方负责 publish)。
//   return: PDU 响应 + 异常码
// ─────────────────────────────────────────────────────────────────────────────
DispatchResult dispatchFull(const SlaveRuntime& slaveRuntime,
                            const uint8_t* pdu, size_t n,
                            bool writable) noexcept;

// 便利函数:已知从站 ID + RegisterBank 引用 + transport,自动取 SlaveRegset.
// 返回 std::nullopt 表示该 slave 未注册或 regs 为空。
std::optional<DispatchResult> dispatchBySlaveId(
    uint8_t slaveId,
    RegisterBank& bank,
    const std::vector<SlaveRuntime>& allSlaves,
    const uint8_t* pdu, size_t n) noexcept;

}  // namespace ens::sim