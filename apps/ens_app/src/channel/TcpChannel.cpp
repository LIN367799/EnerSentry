// TcpChannel.cpp —— 2.1.3 TCP 通道（ENS-LLD-100 §3.3）。状态机：
// open→connectToHost（异步）→Connected（重置退避）→disconnected→退避重连→timer→connectToHost。
#include "TcpChannel.h"

#include <QTcpSocket>
#include <QThread>
#include <QRandomGenerator>

#include <algorithm>
#include <utility>

#ifdef Q_OS_LINUX
  #include <netinet/tcp.h>
  #include <sys/socket.h>
#endif

namespace ens::channel {

TcpChannel::TcpChannel(QObject* parent) : IChannel(parent) {}

TcpChannel::~TcpChannel() { close(); }

bool TcpChannel::open(const ChannelConfig& cfg) {
    if (cfg.type != ChannelType::TCP) {
        m_lastError = QStringLiteral("TcpChannel::open expects TCP config");
        return false;
    }
    if (m_opened) close();                            // 幂等：重复 open 先复位
    m_tcpCfg = std::get<TcpConfig>(cfg.payload);
    m_reconnectBaseMs = cfg.reconnectBaseMs > 0 ? cfg.reconnectBaseMs : 1000;
    m_reconnectMaxMs  = cfg.reconnectMaxMs  > 0 ? cfg.reconnectMaxMs  : 30000;
    m_lastError.clear();

    if (!m_socket) {                                  // 首次或 close 后重建
        m_socket = new QTcpSocket(this);
        m_reconnectTimer = new QTimer(this);
        m_reconnectTimer->setSingleShot(true);

        connect(m_socket, &QTcpSocket::connected,    this, &TcpChannel::onConnected);
        connect(m_socket, &QTcpSocket::disconnected, this, &TcpChannel::onDisconnected);
        connect(m_socket, &QTcpSocket::readyRead,    this, &TcpChannel::onReadyRead);
        connect(m_socket, &QTcpSocket::bytesWritten, this, &TcpChannel::onBytesWritten);
        connect(m_socket, qOverload<QAbstractSocket::SocketError>(&QAbstractSocket::errorOccurred),
                this, &TcpChannel::onErrorOccurred);
        connect(m_reconnectTimer, &QTimer::timeout, this, &TcpChannel::attemptReconnect);
    }

    m_opened = true;
    m_closed = false;
    m_backoffMs = 0;
    m_socket->connectToHost(m_tcpCfg.host, m_tcpCfg.port);   // 异步，首次不走退避
    return true;
}

void TcpChannel::close() {
    if (m_closed) return;                         // 幂等
    m_closed = true;
    m_opened = false;
    m_connectedFlag.store(false, std::memory_order_release);
    if (m_reconnectTimer) m_reconnectTimer->stop();
    if (m_socket) {
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

int TcpChannel::write(const QByteArray& data) {
    if (!isConnected()) {
        m_lastError = QStringLiteral("TcpChannel::write: not connected");
        return -1;
    }
    const qint64 n = m_socket->write(data);       // hand-off：仅入写缓冲即返回
    if (n < 0) {
        m_lastError = m_socket->errorString();
        return -1;
    }
    m_stats.bytesSent.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    return static_cast<int>(n);
}

bool TcpChannel::asyncWrite(const QByteArray& data, WriteCompletedCallback cb) {
    if (!isConnected()) {
        m_lastError = QStringLiteral("TcpChannel::asyncWrite: not connected");
        return false;
    }
    m_pendingWriteCb = std::move(cb);             // 单次 pending（多连接场景由上层串行化）
    const qint64 n = m_socket->write(data);
    if (n < 0) {
        m_pendingWriteCb = nullptr;
        m_lastError = m_socket->errorString();
        return false;
    }
    m_stats.bytesSent.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    return true;
}

QByteArray TcpChannel::read(int maxBytes) {
    if (m_inBuf.isEmpty()) return {};
    const int n = std::min(static_cast<int>(m_inBuf.size()), maxBytes);
    QByteArray out = m_inBuf.left(n);
    m_inBuf.remove(0, n);
    return out;
}

bool TcpChannel::isConnected() const {
    // 线程安全读（flag 由 onConnected/onDisconnected/close 维护，可跨线程调用）
    return m_connectedFlag.load(std::memory_order_acquire);
}

void TcpChannel::setReadCallback(ReadCallback cb)                          { m_readCb  = std::move(cb); }
void TcpChannel::setWriteCompletedCallback(WriteCompletedCallback cb)      { m_writeCb = std::move(cb); }
void TcpChannel::setConnectionChangedCallback(ConnectionChangedCallback cb) { m_connCb  = std::move(cb); }
void TcpChannel::setErrorCallback(ErrorCallback cb)                        { m_errCb   = std::move(cb); }

void TcpChannel::onConnected() {
    m_backoffMs = 0;                              // 重连成功，重置退避（LLD §3.3.2）
    hardenKeepAlive();
    m_connectedFlag.store(true, std::memory_order_release);
    if (m_connCb) m_connCb(true);
    emit connectionChanged(true);
}

void TcpChannel::onDisconnected() {
    m_connectedFlag.store(false, std::memory_order_release);
    if (m_connCb) m_connCb(false);
    emit connectionChanged(false);
    scheduleReconnect();                          // 断线 → 指数退避重连
}

void TcpChannel::onReadyRead() {
    const QByteArray data = m_socket->readAll();
    if (data.isEmpty()) return;
    m_stats.bytesReceived.fetch_add(static_cast<uint64_t>(data.size()), std::memory_order_relaxed);
    m_inBuf.append(data);
    if (m_inBuf.size() > kMaxInBuf) m_inBuf.clear();   // 异常流量保护
    if (m_readCb) m_readCb(data);
    emit dataReceived(data);
}

void TcpChannel::onBytesWritten(qint64 bytes) {
    if (m_pendingWriteCb) {
        auto cb = std::move(m_pendingWriteCb);
        m_pendingWriteCb = nullptr;
        cb(bytes);
    }
    if (m_writeCb) m_writeCb(bytes);
    emit writeCompleted(bytes);
}

void TcpChannel::onErrorOccurred(QAbstractSocket::SocketError err) {
    m_lastError = m_socket ? m_socket->errorString() : QStringLiteral("socket error");
    if (m_errCb) m_errCb(m_lastError);
    emit errorOccurred(m_lastError);
    // 连接建立前的可恢复失败才退避重连；其余（HostNotFound 等）仅上报
    if (!m_opened || m_closed || !m_socket ||
        m_socket->state() != QAbstractSocket::UnconnectedState)
        return;
    if (err == QAbstractSocket::ConnectionRefusedError ||
        err == QAbstractSocket::RemoteHostClosedError ||
        err == QAbstractSocket::NetworkError) {
        scheduleReconnect();
    }
}

void TcpChannel::attemptReconnect() {
    if (!m_opened || m_closed) return;
    if (m_socket && m_socket->state() == QAbstractSocket::UnconnectedState) {
        m_socket->connectToHost(m_tcpCfg.host, m_tcpCfg.port);
    }
}

void TcpChannel::scheduleReconnect() noexcept {
    if (!m_opened || m_closed) return;
    if (m_reconnectTimer && m_reconnectTimer->isActive()) return;   // 幂等：已有定时任务

    // LLD §3.3.2：1→2→4→8→16→30s 封顶；首次 1s
    if (m_backoffMs < m_reconnectMaxMs) {
        m_backoffMs = std::min(m_backoffMs * 2, m_reconnectMaxMs);
        if (m_backoffMs == 0) m_backoffMs = m_reconnectBaseMs;
    }
    // ±10% 抖动（NFR-REL-02 分散多链路重连峰值；QRandomGenerator 线程安全，替代 std::rand）
    const double jitter = m_backoffMs * 0.1 *
        (QRandomGenerator::global()->generateDouble() * 2.0 - 1.0);
    m_reconnectTimer->start(std::max(1, m_backoffMs + static_cast<int>(jitter)));
}

void TcpChannel::hardenKeepAlive() noexcept {
    if (!m_socket) return;
    m_socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
#ifdef Q_OS_LINUX
    const int fd = m_socket->socketDescriptor();
    if (fd < 0) return;
    int idle = 10, interval = 3, probes = 3;      // 10s 空闲 → 每 3s 探测 → 3 次失败判死
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,     sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &probes,   sizeof(probes));
#endif
}

}  // namespace ens::channel