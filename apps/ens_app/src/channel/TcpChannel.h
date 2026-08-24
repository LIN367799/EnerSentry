// TcpChannel.h —— Phase 1 L1 2.1.3：TCP 通道真实实现（ENS-LLD-100 §3.3）。
// 关键语义：
//   - open() 异步 connectToHost；连接/断线经 connectionChanged(bool) 上报
//   - 断线指数退避重连：1→2→4→8→16→30s 封顶 + ±10% 随机抖动（COMM-09 / NFR-REL-02）
//   - 内核 KeepAlive（SO_KEEPALIVE + Linux TCP_KEEPIDLE/INTVL/CNT）；应用级心跳在 L2 PollScheduler
//   - write 非阻塞 hand-off：仅入 QTcpSocket 写缓冲即返回字节数；未连接返回 -1
//   - close 幂等（stop 定时器 + abort + deleteLater）
//   - 字节统计（bytesSent/bytesReceived）在通道层更新；请求/超时/CRC 计数留 L2（Phase 2）
#pragma once

#include "IChannel.h"

#include <QAbstractSocket>
#include <QTimer>

class QTcpSocket;

namespace ens::channel {

class ENS_CHANNEL_API TcpChannel : public IChannel {
    Q_OBJECT
public:
    explicit TcpChannel(QObject* parent = nullptr);
    ~TcpChannel() override;

    bool open(const ChannelConfig& cfg) override;
    void close() override;
    int  write(const QByteArray& data) override;
    bool asyncWrite(const QByteArray& data, WriteCompletedCallback cb) override;
    QByteArray read(int maxBytes = 4096) override;

    bool isConnected() const override;
    const ChannelStats& getStats() const override { return m_stats; }
    QString lastError() const override { return m_lastError; }

    void setReadCallback(ReadCallback cb) override;
    void setWriteCompletedCallback(WriteCompletedCallback cb) override;
    void setConnectionChangedCallback(ConnectionChangedCallback cb) override;
    void setErrorCallback(ErrorCallback cb) override;

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onBytesWritten(qint64 bytes);
    void onErrorOccurred(QAbstractSocket::SocketError err);
    void attemptReconnect();

private:
    void scheduleReconnect() noexcept;
    void hardenKeepAlive() noexcept;

    QTcpSocket* m_socket = nullptr;
    QTimer*     m_reconnectTimer = nullptr;
    TcpConfig   m_tcpCfg{};
    ChannelStats m_stats;
    QString      m_lastError;
    QByteArray   m_inBuf;                          // 接收缓冲：onReadyRead 存、read() 消费（同线程无锁）
    WriteCompletedCallback m_pendingWriteCb;      // asyncWrite 的单次 pending 完成回调
    int  m_backoffMs = 0;                          // 指数退避当前值（LLD §3.3.2）
    int  m_reconnectBaseMs = 1000;                 // 退避初值（open 时取 cfg.reconnectBaseMs）
    int  m_reconnectMaxMs  = 30000;                // 退避封顶（open 时取 cfg.reconnectMaxMs）
    bool m_opened = false;                         // open() 已调用且未 close
    bool m_closed = false;                         // close 幂等标志
};

}  // namespace ens::channel