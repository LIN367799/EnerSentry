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

void RtuSlavePort::setRequestHandler(RequestHandlerWithUnit cb) noexcept {
    m_handlerWithUnit = std::move(cb);
}

std::vector<uint8_t> RtuSlavePort::invokeHandler(uint8_t unitId,
                                                 const std::vector<uint8_t>& reqPdu) {
    if (m_handlerWithUnit) return m_handlerWithUnit(unitId, reqPdu);
    if (m_handler)         return m_handler(reqPdu);
    return dispatchRequest(m_regs, reqPdu.data(), reqPdu.size());
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

    const uint8_t unitId = p[0];
    std::vector<uint8_t> respPdu = invokeHandler(unitId,
        std::vector<uint8_t>(pdu, pdu + pduLen));
    if (respPdu.empty() || !m_port) return;

    // B8:linkEffect 判定（一次性读取,后续 corruptCrc/delayMs 也用）
    FaultEffect eff{};  // 默认 inactive
    if (m_fi != nullptr) {
        eff = m_fi->linkEffect(unitId);
        if (eff.active) {
            if (eff.dropLink) return;                 // 模拟不响应（FR-CTRL-07 断链清锁路径）
            if (eff.corruptByte && respPdu.size() > 3) {
                respPdu[3] ^= 0xFF;                  // LLD-SIM §4.5.4 TCP 等价破坏
            }
        }
    }

    // 响应帧 = addr + respPdu + crc（低字节在前）
    std::vector<uint8_t> resp;
    resp.reserve(1 + respPdu.size() + 2);
    resp.push_back(p[0]);
    resp.insert(resp.end(), respPdu.begin(), respPdu.end());
    const uint16_t crc = ens::core::crc16_modbus(resp.data(), resp.size());
    resp.push_back(static_cast<uint8_t>(crc & 0xFF));
    resp.push_back(static_cast<uint8_t>(crc >> 8));

    if (eff.active) {
        if (eff.corruptCrc) {
            // LLD-SIM §4.5.4:翻 CRC 低字节 → crc16ModbusVerify 必然失败
            resp[resp.size() - 2] ^= 0xFF;
        }
        if (eff.delayMs > 0) {
            // 异步发送:存 m_pendingResp,到时由 onSendDelayTimeout 写出
            m_pendingResp = QByteArray(reinterpret_cast<const char*>(resp.data()),
                                       static_cast<qsizetype>(resp.size()));
            if (m_sendDelayTimer == nullptr) {
                m_sendDelayTimer = new QTimer(this);
                m_sendDelayTimer->setSingleShot(true);
                connect(m_sendDelayTimer, &QTimer::timeout,
                        this, &RtuSlavePort::onSendDelayTimeout);
            }
            m_sendDelayTimer->start(eff.delayMs);
            return;
        }
    }

    m_port->write(reinterpret_cast<const char*>(resp.data()), static_cast<qint64>(resp.size()));
}

void RtuSlavePort::onSendDelayTimeout() {
    if (m_port == nullptr || m_pendingResp.isEmpty()) return;
    m_port->write(m_pendingResp);
    m_pendingResp.clear();
}

}  // namespace ens::sim