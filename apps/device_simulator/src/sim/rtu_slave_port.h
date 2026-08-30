// sim/rtu_slave_port.h —— B4 RTU 虚拟串口从站（ENS-DEV-GUIDE §2B B4 / ENS-SIM-IMP §6.2）。
// 与 ModbusTcpServer 共用 ISlaveTransport 抽象；帧以 CRC-16/MODBUS 收尾（低字节在前）。
// 帧边界：QTimer 帧间静默切帧（interFrameUs：>19200 bps → 1750µs 标准例外，否则 bitsPerChar×3.5/baud）。
// 引擎层串口实现走 QSerialPort（与主程序 SerialChannel 同栈，跨平台 Win32/POSIX 后端统一）。
// open() 失败（com0com 未就绪/端口不存在）返回 false，不崩溃（Track B 踩坑：建议切纯 TCP 回归）。
#pragma once

#include "sim/islave_transport.h"
#include "sim/modbus_slave.h"
#include "sim/FaultInjector.h"  // B8

#include <QByteArray>
#include <QObject>
#include <QSerialPort>
#include <QTimer>

#include <cstdint>
#include <string>

namespace ens::sim {

class RtuSlavePort : public QObject, public ISlaveTransport {
    Q_OBJECT
public:
    explicit RtuSlavePort(std::string portName, uint32_t baud = 115200);
    ~RtuSlavePort() override;

    bool open() noexcept override;
    void close() noexcept override;
    bool isOpen() const noexcept override { return m_port && m_port->isOpen(); }
    void setRequestHandler(RequestHandler cb) override;
    // B6:ModbusSlaveEmulator 通过 unitId(RTU 首字节 = slaveAddress) 路由
    void setRequestHandler(RequestHandlerWithUnit cb) noexcept override;
    // B8:注入 FaultInjector;processFrame 内查 linkEffect 决定 dropLink/corruptCrc/corruptByte/delayMs
    void setFaultInjector(FaultInjector* fi) noexcept override { m_fi = fi; }

    SlaveRegs& regs() noexcept { return m_regs; }

private slots:
    void onReadyRead();
    void onFrameTimeout();
    void onSendDelayTimeout();                            // 帧间静默到期 → 缓冲视为完整帧

private:
    void processFrame(const QByteArray& frame) noexcept;
    // B6:unitId-aware handler 调用
    std::vector<uint8_t> invokeHandler(uint8_t unitId,
                                       const std::vector<uint8_t>& reqPdu);

    QSerialPort* m_port = nullptr;
    QTimer*      m_frameTimer = nullptr;
    QTimer*      m_sendDelayTimer = nullptr;
    QByteArray   m_pendingResp;
    QByteArray   m_rxBuf;
    std::string  m_portName;
    uint32_t     m_baud = 115200;
    int          m_interFrameMs = 2;                  // 帧间静默定时（115200 → 1750µs，取整 2ms）
    SlaveRegs    m_regs;
    RequestHandler         m_handler;
    RequestHandlerWithUnit m_handlerWithUnit;
    FaultInjector*         m_fi = nullptr;        // B6:unitId 路由版本
};

}  // namespace ens::sim