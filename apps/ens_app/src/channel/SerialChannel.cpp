// SerialChannel.cpp —— Phase 1 L1 2.1.2：串口通道实现（ENS-LLD-100 §3.2 / ENS-DEV-GUIDE §2A）。
#include "SerialChannel.h"

#include <QSerialPortInfo>

#include <algorithm>
#include <utility>

namespace ens::channel {

SerialChannel::SerialChannel(QObject* parent) : IChannel(parent) {}

SerialChannel::~SerialChannel() { close(); }

bool SerialChannel::open(const ChannelConfig& cfg) {
    if (cfg.type != ChannelType::Serial) {
        m_lastError = QStringLiteral("SerialChannel::open expects Serial config");
        return false;
    }
    m_serialCfg = std::get<SerialConfig>(cfg.payload);
    if (m_serialCfg.portName.isEmpty()) {
        m_lastError = QStringLiteral("SerialChannel::open: empty portName");
        return false;
    }
    m_lastError.clear();

    if (!m_port) {
        // 端口不存在（com0com 未就绪）→ open 失败，调用方降级
        bool exists = false;
        for (const auto& info : QSerialPortInfo::availablePorts())
            if (info.portName() == m_serialCfg.portName) { exists = true; break; }
        if (!exists) {
            m_lastError = QStringLiteral("SerialChannel::open: port not found: ")
                        + m_serialCfg.portName;
            return false;
        }

        m_port = new QSerialPort(this);
        m_port->setPortName(m_serialCfg.portName);
        if (!m_port->open(QIODevice::ReadWrite)) {
            m_lastError = m_port->errorString();
            delete m_port;
            m_port = nullptr;
            return false;
        }
        m_port->setBaudRate(m_serialCfg.baudRate);
        m_port->setDataBits(QSerialPort::Data8);
        m_port->setStopBits(QSerialPort::OneStop);
        m_port->setParity(QSerialPort::NoParity);
        applyRs485Timing(m_serialCfg.baudRate, m_serialCfg.dataBits,
                         m_serialCfg.stopBits, m_serialCfg.parity);

        connect(m_port, &QSerialPort::readyRead,      this, &SerialChannel::onReadyRead);
        connect(m_port, &QSerialPort::bytesWritten,   this, &SerialChannel::onBytesWritten);
        connect(m_port, &QSerialPort::errorOccurred,  this, &SerialChannel::onErrorOccurred);
    }

    m_opened = true;
    m_closed = false;
    if (m_connCb) m_connCb(true);
    emit connectionChanged(true);
    return true;
}

void SerialChannel::close() {
    if (m_closed) return;                             // 幂等
    m_closed = true;
    m_opened = false;
    if (m_port) {
        m_port->close();
        m_port->deleteLater();
        m_port = nullptr;
    }
    if (m_connCb) m_connCb(false);
    emit connectionChanged(false);
}

int SerialChannel::write(const QByteArray& data) {
    if (!isConnected()) {
        m_lastError = QStringLiteral("SerialChannel::write: not opened");
        return -1;
    }
    const qint64 n = m_port->write(data);             // hand-off：仅入写缓冲即返回
    if (n < 0) {
        m_lastError = m_port->errorString();
        return -1;
    }
    m_stats.bytesSent.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    return static_cast<int>(n);
}

bool SerialChannel::asyncWrite(const QByteArray& data, WriteCompletedCallback cb) {
    if (!isConnected()) {
        m_lastError = QStringLiteral("SerialChannel::asyncWrite: not opened");
        return false;
    }
    m_pendingWriteCb = std::move(cb);
    const qint64 n = m_port->write(data);
    if (n < 0) {
        m_pendingWriteCb = nullptr;
        m_lastError = m_port->errorString();
        return false;
    }
    m_stats.bytesSent.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    return true;
}

QByteArray SerialChannel::read(int maxBytes) {
    if (m_inBuf.isEmpty()) return {};
    const int n = std::min(static_cast<int>(m_inBuf.size()), maxBytes);
    QByteArray out = m_inBuf.left(n);
    m_inBuf.remove(0, n);
    return out;
}

bool SerialChannel::isConnected() const {
    return m_port && m_port->isOpen() && m_opened;
}

void SerialChannel::setReadCallback(ReadCallback cb)                          { m_readCb  = std::move(cb); }
void SerialChannel::setWriteCompletedCallback(WriteCompletedCallback cb)      { m_writeCb = std::move(cb); }
void SerialChannel::setConnectionChangedCallback(ConnectionChangedCallback cb) { m_connCb  = std::move(cb); }
void SerialChannel::setErrorCallback(ErrorCallback cb)                        { m_errCb   = std::move(cb); }

void SerialChannel::onReadyRead() {
    if (!m_port) return;
    const QByteArray data = m_port->readAll();
    if (data.isEmpty()) return;
    m_stats.bytesReceived.fetch_add(static_cast<uint64_t>(data.size()), std::memory_order_relaxed);
    m_inBuf.append(data);                             // 暂存，供 read() 消费
    if (m_readCb) m_readCb(data);
    emit dataReceived(data);
}

void SerialChannel::onBytesWritten(qint64 bytes) {
    if (m_pendingWriteCb) {
        auto cb = std::move(m_pendingWriteCb);
        m_pendingWriteCb = nullptr;
        cb(bytes);
    }
    if (m_writeCb) m_writeCb(bytes);
    emit writeCompleted(bytes);
}

void SerialChannel::onErrorOccurred(QSerialPort::SerialPortError err) {
    m_lastError = m_port ? m_port->errorString() : QStringLiteral("serial error");
    if (m_errCb) m_errCb(m_lastError);
    emit errorOccurred(m_lastError);
    // 串口被拔出/IO 错误 → 标记离线（链路层保障，LLD §3.2.3）
    if (err == QSerialPort::ResourceError || err == QSerialPort::DeviceNotFoundError) {
        if (m_connCb) m_connCb(false);
        emit connectionChanged(false);
    }
}

void SerialChannel::applyRs485Timing(int baudRate, int dataBits, int stopBits,
                                     const QString& parity) noexcept {
    // Modbus Serial Line Protocol V1.02：>19200bps 固定 1.75ms 帧间静默；否则 bitsPerChar×3.5/baud
    if (baudRate > 19200) {
        m_rs485DelayUs = 1750;
    } else {
        const int bitsPerChar = dataBits + (parity == QLatin1String("N") ? 0 : 1) + stopBits;
        m_rs485DelayUs = static_cast<int>((bitsPerChar * 3500000LL) / baudRate);
    }
    // RS485 方向控制由驱动/硬件完成（Linux TIOCSRS485 delay_rts_after_send /
    // Windows RTS_CONTROL_TOGGLE 或自动方向芯片）；com0com 虚拟串口全双工无需切换。
    // **应用层绝不 usleep/QTimer 模拟静默**（LLD §3.2.2 关键约束）——m_rs485DelayUs 仅记录/供驱动。
}

}  // namespace ens::channel