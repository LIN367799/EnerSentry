// sim/rtu_slave_port.cpp —— B4 RTU 虚拟串口从站实现（ENS-SIM-IMP §6.2）。
// 帧处理：收字节 → 重启帧间静默定时器 → 静默到期 → 完整帧 → CRC-16 校验（低字节在前）
//          → 提取 addr+PDU → dispatchRequest（或自定义 handler）→ 响应帧 + CRC 回写。
#include "sim/rtu_slave_port.h"

#include "core/crc16.h"

#include <QSerialPortInfo>

#include <algorithm>
#include <utility>

namespace ens::sim {

RtuSlavePort::RtuSlavePort(std::string portName, uint32_t baud)
    : m_portName(std::move(portName)), m_baud(baud) {
    // 帧间静默：>19200→1750µs（标准例外）；否则 8N1 的 bitsPerChar(9)×3.5/baud；QTimer 向上取整且最小 2ms
    int delayUs = 1750;
    if (baud <= 19200) delayUs = static_cast<int>((9 * 3500000LL) / baud);
    m_interFrameMs = std::max(2, (delayUs + 999) / 1000);
}

RtuSlavePort::~RtuSlavePort() { close(); }

bool RtuSlavePort::open() noexcept {
    if (m_port) return true;                          // 已打开：幂等
    // 端口不存在（com0com 未就绪）→ open 失败，调用方降级（Track B 踩坑）
    const QString name = QString::fromStdString(m_portName);
    bool exists = false;
    for (const auto& info : QSerialPortInfo::availablePorts())
        if (info.portName() == name) { exists = true; break; }
    if (!exists) return false;

    auto* port = new QSerialPort(this);
    port->setPortName(name);
    if (!port->open(QIODevice::ReadWrite)) {
        delete port;
        return false;
    }
    port->setBaudRate(static_cast<qint32>(m_baud));
    port->setDataBits(QSerialPort::Data8);
    port->setStopBits(QSerialPort::OneStop);
    port->setParity(QSerialPort::NoParity);

    m_port = port;
    m_frameTimer = new QTimer(this);
    m_frameTimer->setSingleShot(true);
    m_frameTimer->setInterval(m_interFrameMs);

    connect(m_port, &QSerialPort::readyRead, this, &RtuSlavePort::onReadyRead);
    connect(m_frameTimer, &QTimer::timeout, this, &RtuSlavePort::onFrameTimeout);
    return true;
}

void RtuSlavePort::close() noexcept {
    if (m_frameTimer) { m_frameTimer->stop(); m_frameTimer->deleteLater(); m_frameTimer = nullptr; }
    if (m_port) {
        m_port->close();
        m_port->deleteLater();
        m_port = nullptr;
    }
    m_rxBuf.clear();
}

void RtuSlavePort::setRequestHandler(RequestHandler cb) {
    m_handler = std::move(cb);
}

void RtuSlavePort::onReadyRead() {
    if (!m_port) return;
    m_rxBuf.append(m_port->readAll());
    m_frameTimer->start();                            // 重启静默定时：连续字节在 3.5 字符内算同帧
}

void RtuSlavePort::onFrameTimeout() {
    if (m_rxBuf.isEmpty()) return;
    const QByteArray frame = m_rxBuf;
    m_rxBuf.clear();
    processFrame(frame);
}

void RtuSlavePort::processFrame(const QByteArray& frame) noexcept {
    // RTU 帧 ≥4B：addr + fc + data + crc(2，低字节在前)
    const size_t n = static_cast<size_t>(frame.size());
    if (n < 4) return;

    const uint8_t* p = reinterpret_cast<const uint8_t*>(frame.constData());
    const uint16_t calc = ens::core::crc16_modbus(p, n - 2);
    const uint16_t recv = static_cast<uint16_t>(p[n - 2] | (p[n - 1] << 8));
    if (calc != recv) return;                         // CRC 失败丢弃（FR-SIM-05d 坏帧注入路径）

    // 请求 = addr + PDU（fc + data）
    const uint8_t* pdu = p + 1;
    const size_t pduLen = n - 1 - 2;

    std::vector<uint8_t> respPdu;
    if (m_handler) {
        respPdu = m_handler(std::vector<uint8_t>(pdu, pdu + pduLen));
    } else {
        respPdu = dispatchRequest(m_regs, pdu, pduLen);
    }
    if (respPdu.empty() || !m_port) return;

    // 响应帧 = addr + respPdu + crc（低字节在前）
    std::vector<uint8_t> resp;
    resp.reserve(1 + respPdu.size() + 2);
    resp.push_back(p[0]);                             // 从站地址原样回
    resp.insert(resp.end(), respPdu.begin(), respPdu.end());
    const uint16_t crc = ens::core::crc16_modbus(resp.data(), resp.size());
    resp.push_back(static_cast<uint8_t>(crc & 0xFF));
    resp.push_back(static_cast<uint8_t>(crc >> 8));
    m_port->write(reinterpret_cast<const char*>(resp.data()), static_cast<qint64>(resp.size()));
}

}  // namespace ens::sim