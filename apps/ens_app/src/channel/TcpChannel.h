// TcpChannel.h —— 2.1.3 TCP 通道（ENS-LLD-100 §3.3）。
// open 异步连接；断线指数退避重连（1→2→4→8→16→30s ±10% 抖动）；KeepAlive 加固；
// write 非阻塞 hand-off；close 幂等。字节统计在此层，请求/超时计数留 L2。
#pragma once

#include "IChannel.h"

#include <QAbstractSocket>
#include <QTimer>

#include <atomic>

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

    // 线程安全连接标志（2026-08-31 切片 14 修复）：
    //   QTcpSocket::state() 仅允许在 socket 所在线程调用，而 write() 是 Qt 明确
    //   线程安全的例外 → 采集线程（worker）直接 write 前调 isConnected() 若走
    //   m_socket->state() 是跨线程 UB。故 onConnected/onDisconnected/close 维护
    //   原子标志，isConnected() 读标志（等效：onConnected 时 state==Connected，
    //   onDisconnected 时 state==Unconnected）。
    std::atomic<bool> m_connectedFlag{false};
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

    static constexpr int kMaxInBuf = 1 << 20;         // 接收缓冲上限（异常流量保护）

    QTcpSocket* m_socket = nullptr;
    QTimer*     m_reconnectTimer = nullptr;
    TcpConfig   m_tcpCfg{};
    ChannelStats m_stats;
    QString      m_lastError;
    QByteArray   m_inBuf;                            // 接收缓冲（IO 线程存、read() 取，单线程无锁）
    WriteCompletedCallback m_pendingWriteCb;         // asyncWrite 单次 pending
    int  m_backoffMs = 0;                            // 退避当前值（LLD §3.3.2）
    int  m_reconnectBaseMs = 1000;                   // 退避初值（cfg.reconnectBaseMs）
    int  m_reconnectMaxMs  = 30000;                  // 退避封顶（cfg.reconnectMaxMs）
    bool m_opened = false;
    bool m_closed = false;
};

}  // namespace ens::channel