// SerialChannel.h —— 2.1.2 串口通道（ENS-LLD-100 §3.2）。
// QSerialPort 跨平台封装；RS485 帧间静默 ≥3.5 字符（>19200bps 例外 1750µs），方向由驱动/硬件
// 完成、应用层禁模拟静默；write 非阻塞；close 幂等。字节统计在此层，请求/超时计数留 L2。
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

    static constexpr int kMaxInBuf = 1 << 20;         // 接收缓冲上限（异常流量保护）

    QSerialPort* m_port = nullptr;
    SerialConfig m_serialCfg{};
    int          m_rs485DelayUs = 1750;               // 3.5 字符静默（µs，供驱动）
    ChannelStats m_stats;
    QString      m_lastError;
    QByteArray   m_inBuf;                             // 接收缓冲（IO 线程存、read() 取）
    WriteCompletedCallback m_pendingWriteCb;          // asyncWrite 单次 pending
    bool m_opened = false;
    bool m_closed = false;
};

}  // namespace ens::channel