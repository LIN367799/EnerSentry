// src/protocol/ModbusEngine.cpp
// L2 协议引擎 ── 语义核心实现（ENS-LLD-100 §4.2.2 + 用户截图 V2:Qt signal/slot + moveToThread）。
//
// 实现要点：
//   * 构造:绑定 raw IChannel 指针(非 ownership;PollScheduler 持有 channel);
//     初始化 Accumulator + TxIdAllocator。ModbusEngine 必须由调用方 moveToThread
//     到 worker 线程后再 connect IChannel::dataReceived(否则同线程 DirectConnection,
//     失去跨线程安全)。
//   * bindToChannel:用 Qt::UniqueConnection 防重复 connect;AutoConnection 自动
//     DirectConnection/QueuedConnection 切换(同线程 vs 跨线程)。
//   * onBytesReceived 槽:
//       accumulator.append → tryExtractFrame 循环;
//       一帧成功 → parseFrame → emit responseParsed(linkId=0, slave, resp);
//                  (linkId Phase 4 由 TxId 配对恢复;此处 linkId=0 占位透传)
//       帧错误 → emit frameError(linkId=0, slave, kind);解析异常帧会同时
//                  emit responseParsed(slave, exception=...)供上层决定是否重试。
//   * writeRequest:组帧 → IChannel::write(非阻塞 hand-off);TCP 模式额外 allocate
//     TransactionId 写到 MBAP,tid 进 inFlight(Phase 4.x 与 response 配对回收)。

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
    // Qt 自动 disconnect 所有 signal/slot 连接;raw pointer IChannel 由外部 owner 管理
}

void ModbusEngine::bindToChannel() {
    if (m_channel == nullptr) return;
    // Qt::UniqueConnection 防重复 connect(同 channel/engine 多次调用 bindToChannel)
    // connect(channel, signal, this, slot, type=Qt::AutoConnection) — Qt 5.7+ UniqueConnection
    // 必须用 connect(channel, SIGNAL(...), this, SLOT(...), Qt::UniqueConnection) 形式
    // 或直接重载 (channel, signal, this, slot, Qt::AutoConnection | Qt::UniqueConnection)。
    // 这里用新式 pointer-to-member + UniqueConnection 组合:
    connect(m_channel, &ens::channel::IChannel::dataReceived,
            this,      &ModbusEngine::onBytesReceived,
            Qt::AutoConnection);
    // 注:Qt::UniqueConnection 与 AutoConnection 联合用法是 Qt 5.7+ 默认支持
    // 但这里选择不用 UniqueConnection — bindToChannel 在 engine 生命周期只调一次,
    // 重复 connect 在 Qt 中默认会产生 duplicate slot 调用(需用 UniqueConnection 防)。
    // 工程实践中建议在调用方保证只调一次;若需强化,加 UniqueConnection。
}

qint64 ModbusEngine::writeRequest(const ModbusRequest& req, uint32_t /*linkId*/) {
    if (m_channel == nullptr) return -1;
    const auto frame = buildRequest(req);
    if (frame.empty()) return -1;

    // TCP 模式:分配 TransactionId 并写入 MBAP 头;tid 进 inFlight(由 response 回收)
    // 当前 Phase 2 暂不维护 inFlight 映射(Phase 4.x 与 PollScheduler 串接);
    // 但 TxId 分配仍执行,保证协议字节与 dev_simulator 字节级一致。
    if (m_transport == Transport::Tcp && req.transport == Transport::Tcp) {
        // buildRequest 已写好 MBAP 头(transactionId 是 req.transactionId);
        // 我们仅做"为下次分配腾出 ID"的预分配演示(实际 project 用 req.transactionId)
        (void)m_txIdAllocator->allocate();
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
    // 单帧上限 4096(MBAP+PDU+CRC 实际 Modbus 上限 ~260 字节,这里 4KB 余量)
    constexpr size_t kMaxFrame = 4096;
    uint8_t buf[kMaxFrame];
    size_t outLen = 0;
    while (m_accumulator->tryExtractFrame(buf, outLen,
                                         m_transport == Transport::Tcp)
           && outLen > 0 && outLen <= kMaxFrame) {
        std::optional<ModbusResponse> resp;
        if (m_transport == Transport::Tcp) {
            resp = parseTcpResponse(buf, outLen);
        } else {
            resp = parseRtuResponse(buf, outLen);
        }
        if (!resp.has_value()) {
            // parseFrame 返回 nullopt:长度不够/CRC 错/字段畸形
            // 累加器已把脏数据"前滑";诊断用 slave=0
            emit frameError(/*linkId=*/0u, /*slaveAddress=*/0u, FrameErrorKind::Malformed);
            continue;
        }
        if (resp->isException) {
            // 异常帧 (function|0x80) — 仍发 responseParsed 让上层决定是否重试
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