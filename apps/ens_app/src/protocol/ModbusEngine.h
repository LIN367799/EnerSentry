// src/protocol/ModbusEngine.h
// L2 协议引擎 ── 语义核心（ENS-DEV-GUIDE §3A 3.1.3 / ENS-LLD-100 §4.2.2）。
//
// 责任边界：
//   * 持有 ModbusStreamAccumulator + TransactionIdAllocator,作为 L1 字节流 → L3 Sample
//     之间的"语义核心";串起 channel → accumulator → frame → response 完整链路。
//   * onBytesReceived 槽接 IChannel::dataReceived(QByteArray) — ModbusEngine 通常
//     moveToThread 到 worker 线程,Qt::AutoConnection 自动选 QueuedConnection 跨线程派发,
//     slot 在 worker 线程 context 内执行(无锁)。
//   * writeRequest 组帧后 IChannel::write;TCP 模式下分配 TransactionId 写入 MBAP,
//     响应解析成功后按 tid 释放回收(位图不泄漏)。inFlight 配对路由属 Phase 4。
//   * 解析成功 → emit responseParsed(linkId, slave, ModbusResponse);
//     解析失败 → emit frameError(linkId, slave, kind) — PollScheduler/监控层订阅。
//
// ⚠ 与 LLD-100 §4.2.2 的差异(用户截图 V2 指令):
//   LLD §4.2.2 用 setReadCallback()(同步 callback,无法跨线程);
//   本实现用 Qt signal/slot connect + moveToThread(worker 线程 context),
//   跨线程安全由 Qt::AutoConnection(QueuedConnection)保证。
//
// 不做(Phase 2 后续 / Phase 3 收口):
//   * Sample.value 与 PointTable 缩放还原 — 留给 PollScheduler 3.1.4 + L1SnapshotStore 4.x。
//   * 超时重发/熔断/降级 — 3.1.4 PollScheduler 职责。
//   * 多链路 ID → linkId 路由(同一 IChannel 多个并发请求的 tid 配对) —
//     当前协议层只暴露 linkId(由调用方 PollScheduler 提供),engine 内仅透传;
//     TxId 分配/回收路径已就位,Phase 4 串接。

#pragma once

#include "ModbusFrame.h"

#include <cstddef>
#include <cstdint>

#include <memory>

#include <QByteArray>
#include <QObject>
#include <QString>

namespace ens::channel {
class IChannel;
}

namespace ens::protocol {

class ModbusStreamAccumulator;
class TransactionIdAllocator;

// ─────────────────────────────────────────────────────────────────────────────
// 帧错误类型(枚举 emit frameError 时用)
// ─────────────────────────────────────────────────────────────────────────────
enum class FrameErrorKind : uint8_t {
    Crc         = 0,   // RTU CRC 校验失败
    Length      = 1,   // 长度不够/超长(MBAP Length 域错配)
    Malformed   = 2,   // 解析时结构畸形(PDU 字段越界等)
    Timeout     = 3,   // 超时未收到响应(由 PollScheduler 触发,此处保留枚举值)
    Exception   = 4,   // 从站返回 0x80|FC 异常帧(可重试语义留给上层)
    Unsupported = 5,   // 收到未知功能码
};

// ─────────────────────────────────────────────────────────────────────────────
// ModbusEngine ── 协议语义核心(QObject 派生,可 moveToThread)
// ─────────────────────────────────────────────────────────────────────────────
class ModbusEngine : public QObject {
    Q_OBJECT

public:
    explicit ModbusEngine(ens::channel::IChannel* channel,
                          Transport transport,
                          QObject* parent = nullptr);
    ~ModbusEngine() override;

    ModbusEngine(const ModbusEngine&) = delete;
    ModbusEngine& operator=(const ModbusEngine&) = delete;
    ModbusEngine(ModbusEngine&&) = delete;
    ModbusEngine& operator=(ModbusEngine&&) = delete;

    /// 绑定 IChannel::dataReceived → onBytesReceived(slot)。
    /// 调用方保证只调一次;重复 connect 会产生重复投递。
    void bindToChannel();

    /// 下发 Modbus 请求(组帧 + IChannel::write)。
    ///   linkId —— 调用方提供的链路 ID(PollScheduler 分配),透传至 responseParsed。
    ///   成功返回 ≥0 字节数;失败返回 -1(QByteArray::size() 域内)。
    qint64 writeRequest(const ModbusRequest& req, uint32_t linkId);

    /// 帧错误类型到字符串(诊断 / 日志用)
    static QString frameErrorKindToString(FrameErrorKind kind) noexcept;

    // ── 诊断 ──
    Transport transport() const noexcept { return m_transport; }

signals:
    /// 解析成功时发出(linkId 由 writeRequest 时透传)
    void responseParsed(uint32_t linkId, uint8_t slaveAddress,
                        const ModbusResponse& resp);

    /// 帧错误时发出(长度不符 / 畸形 / 异常帧 / 未知 FC 等;CRC 错由累加器内吞,不 emit)
    void frameError(uint32_t linkId, uint8_t slaveAddress,
                    FrameErrorKind kind);

public slots:
    /// 接 IChannel::dataReceived(QByteArray) — worker 线程 context(经 Qt QueuedConnection)。
    void onBytesReceived(const QByteArray& data);

private:
    // ── 内部 ──
    // 注意:IChannel 由外部 owner 管理(Phase 3.x PollScheduler 持有),
    // 这里只持有 raw 指针 + connect。ModbusEngine destroy 时会自动 disconnect。
    ens::channel::IChannel* m_channel = nullptr;       // no ownership
    Transport              m_transport;
    // pImpl 隐藏 Accumulator / TxIdAllocator 实现细节(头里只前向声明)
    std::unique_ptr<ModbusStreamAccumulator>  m_accumulator;
    std::unique_ptr<TransactionIdAllocator>   m_txIdAllocator;
};

// ModbusResponse 跨线程信号需要(protocol::ModbusResponse 含 std::vector,
// Qt::QueuedConnection 必须知道类型名)。
Q_DECLARE_METATYPE(ens::protocol::ModbusResponse)

}  // namespace ens::protocol