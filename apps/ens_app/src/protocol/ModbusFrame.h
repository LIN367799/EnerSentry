// src/protocol/ModbusFrame.h
// L2 协议引擎 ── Modbus 报文结构与组帧/拆帧（ENS-LLD-100 §4.1 / §4.2.2 / ADR-LLD-04）。
//
// 责任边界：
//   * buildRequest  ── 静态契约层输出：从 ModbusRequest 组出 RTU/TCP 字节序列（含 CRC/MBAP）。
//   * parseResponse ── 静态契约层输入：把 ModbusStreamAccumulator 已切分好的完整帧还原为
//                      ModbusResponse；CRC/MBAP 校验失败/帧结构不合法返回 std::nullopt。
//   * 流式拼帧（粘包/半包）由 ModbusStreamAccumulator 负责；本头**只**对完整帧做语义解析。
//
// 关键设计：
//   * 零 Qt 依赖，纯 C++17（protocol 是 STATIC，跨 TU 复用，不污染消费者头依赖）。
//   * 全部函数 noexcept，不抛异常（热路径用,异常路径由 ModbusEngine 转 frameError 信号）。
//   * 字节序：所有 16-bit 字段按网络序（大端）；CRC 低字节在前落帧,parse 时按同序读回。
//   * 异常帧独占识别：`functionCode & 0x80` → 立即 isException=true + exceptionCode=byte[2]。

#pragma once

#include "Crc16.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ens::protocol {

/// 传输层方言：与 IChannel/ChannelConfig 一致，RTU 走 CRC、TCP 走 MBAP。
enum class Transport : uint8_t {
    Rtu = 0,
    Tcp = 1,
};

/// Modbus 主请求：覆盖 FC01/02/03/04 读 + FC05/06/0F/10 写。
/// @design-intent 把"sclae/byteOrder/unit conversion"职责留给 ModbusEngine/PointTable(3.1.5)，
///                本层仅承载传输侧字节契约,不引入单位/缩放概念。
struct ModbusRequest {
    Transport transport      = Transport::Rtu;
    uint16_t  transactionId  = 0;     // 仅 TCP 有效；RTU 固定为 0
    uint8_t   slaveAddress   = 0;     // 1~247（HLD §3.1.2 链路层路由）
    uint8_t   functionCode   = 0;     // 0x01~0x10
    uint16_t  startingAddress = 0;
    uint16_t  quantity        = 0;    // 读类：寄存器/线圈数；FC10: 寄存器数；FC0F: 线圈数
    // 写多类 payload：FC10 用 registerValues；FC0F 用 coilBytes（位打包后字节流，已由调用方处理）。
    std::vector<uint16_t> registerValues;
    std::vector<uint8_t>  coilBytes;
};

/// Modbus 解析结果：正常帧 / 异常帧 二态合一（isException 区分）。
struct ModbusResponse {
    Transport transport      = Transport::Rtu;
    uint16_t  transactionId  = 0;     // 仅 TCP 解析时回填
    uint8_t   slaveAddress   = 0;
    uint8_t   functionCode   = 0;     // 异常帧: 原 FC | 0x80
    bool      isException    = false;
    uint8_t   exceptionCode  = 0;     // 仅 isException
    // 正常读帧：FC03/04 按 big-endian 解析为 u16 列表；FC01/02 按位打包原字节流。
    // 正常写帧：FC05/06/0F/10 解析后的回显（与请求字段一致）,便于上层链路审计。
    std::vector<uint16_t> registerValues;
    std::vector<uint8_t>  coilBytes;
    ModbusResponse() = default;
    ModbusResponse(Transport t, uint8_t unit, uint8_t fc)
        : transport(t), slaveAddress(unit), functionCode(fc) {}
};

// ── 组帧：ModbusRequest → 字节序列 ──

/// 组出 RTU 帧（含 CRC-16/MODBUS,低字节在前）或 TCP 帧（MBAP 7 字节 + PDU）。
/// 非法输入（如 FC05 的 data 不为 0xFF00/0x0000）直接返回空 vector + 错误码由调用方读 errno-style 字段。
/// 返回字节数：
///   FC01/02/03/04 读类 → 6 + 2(CRC);RTU = 8 字节
///   FC05 写单线圈   → 6 + 2(CRC);RTU = 8 字节 (data=0xFF00=ON / 0x0000=OFF)
///   FC06 写单寄存器 → 6 + 2(CRC);RTU = 8 字节
///   FC0F 写多线圈   → 7 + ceil(qty/8) + pad + 2(CRC);RTU
///   FC10 写多寄存器 → 9 + 2*qty + 2(CRC);RTU
/// TCP 在 RTU 基础上砍 2 字节 CRC、增 7 字节 MBAP 头。
std::vector<uint8_t> buildRequest(const ModbusRequest& req) noexcept;

/// ── 拆帧：字节序列（已被 ModbusStreamAccumulator 切好）→ ModbusResponse ──

/// 解析 RTU 帧（完整帧,CRC 已在前置阶段校验过;此处仍冗余校验一次以兜底）。
/// TCP 帧调用 parseTcpResponse。
/// 任何不合法（长度不够、CRC 错、字段越界）返回 std::nullopt。
std::optional<ModbusResponse> parseResponse(const uint8_t* buf, size_t len) noexcept;

/// TCP 帧专用解析入口（上层已知走 TCP,避免内层 if isTcp 重复分支）。
std::optional<ModbusResponse> parseTcpResponse(const uint8_t* buf, size_t len) noexcept;

/// RTU 帧专用解析入口。
std::optional<ModbusResponse> parseRtuResponse(const uint8_t* buf, size_t len) noexcept;

/// 诊断辅助：根据 functionCode 在没有 payload 时给出期望最小响应长度（不含 CRC/MBAP）。
/// 用于 ModbusEngine 在累加器外"知道还差几个字节"。
size_t expectedResponseBodySize(uint8_t functionCode, uint16_t quantity) noexcept;

}  // namespace ens::protocol
