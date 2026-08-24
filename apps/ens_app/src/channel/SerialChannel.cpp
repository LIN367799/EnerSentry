// SerialChannel.cpp —— Phase 1 L1：SerialChannel 最小骨架（ENS-LLD-100 §3.2 / ENS-DEV-GUIDE §2A 2.1.1）。
// 本 PR 仅 override 全部虚函数保证编译通过；QSerialPort 真实收发留待 2.1.2。
// 不变量：
//   - open 成功后 m_opened=true、m_closed=false
//   - close 幂等（m_closed 置位后再次调用立即返回）
//   - 未 open 时 write/asyncWrite 返回 -1/false + lastError（防误用）
#include "SerialChannel.h"

#include <utility>

namespace ens::channel {

SerialChannel::SerialChannel(QObject* parent) : IChannel(parent) {}

SerialChannel::~SerialChannel() = default;

bool SerialChannel::open(const ChannelConfig& cfg) {
    if (cfg.type != ChannelType::Serial) {
        m_lastError = QStringLiteral("SerialChannel::open expects Serial config");
        return false;
    }
    m_serialCfg = std::get<SerialConfig>(cfg.payload);
    m_lastError.clear();
    m_opened = true;
    m_closed = false;
    // 2.1.2 落地：new QSerialPort(m_serialCfg.portName) + setBaudRate + RS485 ioctl
    return true;
}

void SerialChannel::close() {
    if (m_closed) return;                          // 幂等
    m_closed = true;
    m_opened  = false;
    // 2.1.2 落地：m_port->close() + deleteLater()
}

int SerialChannel::write(const QByteArray& /*data*/) {
    if (!m_opened) {
        m_lastError = QStringLiteral("SerialChannel::write: not opened");
        return -1;
    }
    // 2.1.2 落地：m_port->write(data)（hand-off，瞬时返回）
    return -1;                                     // stub
}

bool SerialChannel::asyncWrite(const QByteArray& /*data*/, WriteCompletedCallback /*cb*/) {
    if (!m_opened) {
        m_lastError = QStringLiteral("SerialChannel::asyncWrite: not opened");
        return false;
    }
    return false;                                  // stub
}

QByteArray SerialChannel::read(int /*maxBytes*/) {
    if (!m_opened) return {};
    return {};                                     // stub
}

bool SerialChannel::isConnected() const {
    return m_opened && !m_closed;
}

void SerialChannel::setReadCallback(ReadCallback cb)                          { m_readCb  = std::move(cb); }
void SerialChannel::setWriteCompletedCallback(WriteCompletedCallback cb)      { m_writeCb = std::move(cb); }
void SerialChannel::setConnectionChangedCallback(ConnectionChangedCallback cb) { m_connCb  = std::move(cb); }
void SerialChannel::setErrorCallback(ErrorCallback cb)                        { m_errCb   = std::move(cb); }

}  // namespace ens::channel