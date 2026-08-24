// TcpChannel.cpp —— Phase 1 L1：TcpChannel 最小骨架（ENS-LLD-100 §3.3 / ENS-DEV-GUIDE §2A 2.1.1）。
// 本 PR 仅 override 全部虚函数；QTcpSocket + 指数退避留待 2.1.3。
// 不变量同 SerialChannel：open 成功后 m_opened=true、close 幂等、未 open 时 IO 返回 -1/false。
#include "TcpChannel.h"

#include <utility>

namespace ens::channel {

TcpChannel::TcpChannel(QObject* parent) : IChannel(parent) {}

TcpChannel::~TcpChannel() = default;

bool TcpChannel::open(const ChannelConfig& cfg) {
    if (cfg.type != ChannelType::TCP) {
        m_lastError = QStringLiteral("TcpChannel::open expects TCP config");
        return false;
    }
    m_tcpCfg = std::get<TcpConfig>(cfg.payload);
    m_lastError.clear();
    m_opened = true;
    m_closed = false;
    // 2.1.3 落地：new QTcpSocket + connectToHost(m_tcpCfg.host, m_tcpCfg.port) + 指数退避定时器
    return true;
}

void TcpChannel::close() {
    if (m_closed) return;
    m_closed = true;
    m_opened  = false;
    // 2.1.3 落地：m_socket->abort() + deleteLater()（注意同时停止 m_reconnectTimer）
}

int TcpChannel::write(const QByteArray& /*data*/) {
    if (!m_opened) {
        m_lastError = QStringLiteral("TcpChannel::write: not opened");
        return -1;
    }
    return -1;                                     // stub
}

bool TcpChannel::asyncWrite(const QByteArray& /*data*/, WriteCompletedCallback /*cb*/) {
    if (!m_opened) {
        m_lastError = QStringLiteral("TcpChannel::asyncWrite: not opened");
        return false;
    }
    return false;                                  // stub
}

QByteArray TcpChannel::read(int /*maxBytes*/) {
    if (!m_opened) return {};
    return {};
}

bool TcpChannel::isConnected() const {
    return m_opened && !m_closed;
}

void TcpChannel::setReadCallback(ReadCallback cb)                          { m_readCb  = std::move(cb); }
void TcpChannel::setWriteCompletedCallback(WriteCompletedCallback cb)      { m_writeCb = std::move(cb); }
void TcpChannel::setConnectionChangedCallback(ConnectionChangedCallback cb) { m_connCb  = std::move(cb); }
void TcpChannel::setErrorCallback(ErrorCallback cb)                        { m_errCb   = std::move(cb); }

}  // namespace ens::channel