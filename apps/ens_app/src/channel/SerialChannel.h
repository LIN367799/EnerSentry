// SerialChannel.h —— Phase 1 L1：SerialChannel 抽象骨架（ENS-LLD-100 §3.2）。
// 2.1.1 仅 override 全部虚函数（最小可编译骨架）；
// 2.1.2 引入 QSerialPort + RS485 DE/RE 方向控制 + 3.5 字符时序（interFrameDelayUs 表 + delay_rts_after_send）。
#pragma once

#include "IChannel.h"

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

private:
    SerialConfig m_serialCfg{};
    ChannelStats m_stats;
    QString      m_lastError;
    bool         m_opened = false;     // open 成功且未 close
    bool         m_closed = false;     // close 幂等标志
};

}  // namespace ens::channel