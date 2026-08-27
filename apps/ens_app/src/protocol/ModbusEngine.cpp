// src/protocol/ModbusEngine.cpp
// L2 协议引擎 ── 语义核心实现（ENS-LLD-100 §4.2.2：Qt signal/slot + moveToThread）。
//
// 实现要点：
//   * 构造:持有 raw IChannel 指针(非 ownership);初始化 Accumulator + TxIdAllocator。
//     engine 由调用方 moveToThread 到 worker 线程后再 connect dataReceived。
//   * onBytesReceived 槽:accumulator.append → tryExtractFrame 循环 → parse → emit;
//     TCP 响应按 tid 释放 TxId;异常帧同时发 responseParsed(exception) + frameError(Exception)。
//   * writeRequest:TCP 模式先 allocate TxId 写入 MBAP,组帧失败或位图耗尽时返 -1。

#include "ModbusEngine.h"

#include "ModbusFrame.h"
#include "ModbusStreamAccumulator.h"
#include "TransactionIdAllocator.h"

#include "IChannel.h"   // src/channel 是 ens_protocol PRIVATE include 根(apps/ens_app/CMakeLists.txt)

#include <QMetaType>

namespace ens::protocol {

ModbusEngine::ModbusEngine(ens::channel::IChannel* channel,
                           Transport transport,
                           QObject* parent)
    : QObject(parent),
      m_channel(channel),
      m_transport(transport),
      m_accumulator(std::make_unique<ModbusStreamAccumulator>()),
      m_txIdAllocator(std::make_unique<TransactionIdAllocator>()) {}

ModbusEngine::~ModbusEngine() {
    // Qt 自动 disconnect 全部连接;IChannel 由外部 owner 管理
}

void ModbusEngine::bindToChannel() {
    if (m_channel == nullptr) return;
    // AutoConnection:同线程 Direct,跨线程 Queued。调用方保证只调一次。
    connect(m_channel, &ens::channel::IChannel::dataReceived,
            this,      &ModbusEngine::onBytesReceived,
            Qt::AutoConnection);
}

qint64 ModbusEngine::writeRequest(const ModbusRequest& req, uint32_t /*linkId*/) {
    if (m_channel == nullptr) return -1;

    ModbusRequest r = req;
    if (m_transport == Transport::Tcp && r.transport == Transport::Tcp) {
        const uint16_t tid = m_txIdAllocator->allocate();
        if (tid == TransactionIdAllocator::INVALID_ID) return -1;   // 位图耗尽
        r.transactionId = tid;
    }
    const auto frame = buildRequest(r);
    if (frame.empty()) {
        if (r.transactionId != 0) m_txIdAllocator->release(r.transactionId);
        return -1;
    }

    const QByteArray bytes(reinterpret_cast<const char*>(frame.data()),
                           static_cast<int>(frame.size()));
    return m_channel->write(bytes);
}

void ModbusEngine::onBytesReceived(const QByteArray& data) {
    if (data.isEmpty()) return;
    const auto* raw = reinterpret_cast<const uint8_t*>(data.constData());
    const size_t len = static_cast<size_t>(data.size());

    m_accumulator->append(raw, len);

    // 累加器可能一次喂入多帧(碎片重组 / 粘包);循环提取直到空。
    // Modbus 帧上限 ~260 字节,4KB 栈缓冲留足余量。
    constexpr size_t kMaxFrame = 4096;
    uint8_t buf[kMaxFrame];
    size_t outLen = 0;
    while (m_accumulator->tryExtractFrame(buf, outLen,
                                         m_transport == Transport::Tcp)
           && outLen > 0) {
        std::optional<ModbusResponse> resp;
        if (m_transport == Transport::Tcp) {
            resp = parseTcpResponse(buf, outLen);
        } else {
            resp = parseRtuResponse(buf, outLen);
        }
        if (!resp.has_value()) {
            // 长度不够/CRC 错/字段畸形:累加器已前滑脏数据,诊断用 slave=0
            emit frameError(/*linkId=*/0u, /*slaveAddress=*/0u, FrameErrorKind::Malformed);
            continue;
        }
        if (m_transport == Transport::Tcp) {
            m_txIdAllocator->release(resp->transactionId);   // 回收 TxId
        }
        if (resp->isException) {
            // 异常帧 (function|0x80):仍发 responseParsed 让上层决定是否重试
            emit responseParsed(/*linkId=*/0u, resp->slaveAddress, *resp);
            emit frameError(/*linkId=*/0u, resp->slaveAddress, FrameErrorKind::Exception);
            continue;
        }
        emit responseParsed(/*linkId=*/0u, resp->slaveAddress, *resp);
    }
}

QString ModbusEngine::frameErrorKindToString(FrameErrorKind kind) noexcept {
    switch (kind) {
        case FrameErrorKind::Crc:         return QStringLiteral("Crc");
        case FrameErrorKind::Length:      return QStringLiteral("Length");
        case FrameErrorKind::Malformed:   return QStringLiteral("Malformed");
        case FrameErrorKind::Timeout:     return QStringLiteral("Timeout");
        case FrameErrorKind::Exception:   return QStringLiteral("Exception");
        case FrameErrorKind::Unsupported: return QStringLiteral("Unsupported");
    }
    return QStringLiteral("Unknown");
}

}  // namespace ens::protocol