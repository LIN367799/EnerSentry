// TcpChannel.h —— Phase 1 L1：TcpChannel 抽象骨架（ENS-LLD-100 §3.3）。
// 2.1.1 仅 override 全部虚函数；
// 2.1.3 引入 QTcpSocket + 指数退避重连（1→2→4→8→16→30s + ±10% 抖动）+ KeepAlive + 半开连接识别。
#pragma once

#include "IChannel.h"

namespace ens::channel {

class ENS_CHANNEL_API TcpChannel : public IChannel {
    Q_OBJECT
public:
    explicit TcpChannel(QObject* parent = nullptr);
    ~TcpChannel() override;

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
    TcpConfig    m_tcpCfg{};
    ChannelStats m_stats;
    QString      m_lastError;
    bool         m_opened = false;
    bool         m_closed = false;
};

}  // namespace ens::channel