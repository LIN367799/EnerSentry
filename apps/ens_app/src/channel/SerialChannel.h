// SerialChannel.h —— Phase 1 L1 2.1.2：串口通道真实实现（ENS-LLD-100 §3.2）。
// 关键语义：
//   - QSerialPort 打开 + 参数（baud/dataBits/stopBits/parity），跨平台 Win32/POSIX 由 Qt 后端统一
//   - RS485 半双工：帧间静默 ≥3.5 字符（>19200bps 标准例外 1750µs）；方向控制由驱动/硬件完成，
//     应用层**禁用 usleep/QTimer 模拟静默**（LLD §3.2.2 关键约束）；com0com 虚拟串口全双工无需切换
//   - write 非阻塞 hand-off；onReadyRead 存 m_inBuf + dataReceived；close 幂等（对齐 TcpChannel 模式）
//   - 字节统计通道层更新；请求/超时/CRC 计数留 L2
#pragma once

#include "IChannel.h"

#include <QSerialPort>
#include <QString>

namespace ens::channel {

class ENS_CHANNEL_API SerialChannel : public IChannel {
    Q_OBJECT
public:
    explicit SerialChannel(QObject* parent = nullptr);
    ~SerialChannel() override;

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
    void onReadyRead();
    void onBytesWritten(qint64 bytes);
    void onErrorOccurred(QSerialPort::SerialPortError err);

private:
    void applyRs485Timing(int baudRate, int dataBits, int stopBits, const QString& parity) noexcept;

    QSerialPort* m_port = nullptr;
    SerialConfig m_serialCfg{};
    int          m_rs485DelayUs = 1750;               // 3.5 字符静默（µs）
    ChannelStats m_stats;
    QString      m_lastError;
    QByteArray   m_inBuf;                             // 接收缓冲：onReadyRead 存、read() 消费
    WriteCompletedCallback m_pendingWriteCb;          // asyncWrite 单次 pending
    bool m_opened = false;
    bool m_closed = false;
};

}  // namespace ens::channel