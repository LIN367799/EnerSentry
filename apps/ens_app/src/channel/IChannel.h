// IChannel.h —— Phase 1 L1：IChannel 纯虚抽象（ENS-LLD-100 §3.1）。
// 设计约束：
//   - 派生 QObject：IO 线程 emit signals，跨线程 signal/slot 投递天然安全
//   - close 必须幂等（二次调用不抛、不重复释放，RAII + m_closed 标志）
//   - write 非阻塞 hand-off：仅入发送队列即返回（RS485 串行由 PollScheduler 状态机保证，非内部阻塞）
//   - getStats() 返回原子快照，跨线程读取无锁
//   - 4 个 m_xxxCb 由基类 protected 统一持有，子类 setXxx = 0 强制显式注入（避免子类各自重复 4 字段）
// 2.1.1 仅定义抽象 + signals；SerialChannel/TcpChannel 本 PR 提供最小 override 骨架，
// 真实收发在 2.1.2/2.1.3（ENS-DEV-GUIDE §2A）。
#pragma once

#include "ens/export.hpp"
#include "ChannelConfig.h"
#include "ChannelStats.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace ens::channel {

using ReadCallback              = std::function<void(const QByteArray& data)>;
using WriteCompletedCallback    = std::function<void(qint64 bytesWritten)>;
using ConnectionChangedCallback = std::function<void(bool connected)>;
using ErrorCallback             = std::function<void(const QString& errorMessage)>;

class ENS_CHANNEL_API IChannel : public QObject {
    Q_OBJECT
public:
    explicit IChannel(QObject* parent = nullptr) : QObject(parent) {}
    ~IChannel() override = default;

    IChannel(const IChannel&)            = delete;
    IChannel& operator=(const IChannel&) = delete;
    IChannel(IChannel&&)                 = delete;
    IChannel& operator=(IChannel&&)      = delete;

    // ── 生命周期 ──
    virtual bool open(const ChannelConfig& cfg) = 0;
    virtual void close() = 0;                       // 必须幂等

    // ── I/O 操作 ──
    virtual int  write(const QByteArray& data) = 0;                          // 非阻塞 hand-off
    virtual bool asyncWrite(const QByteArray& data, WriteCompletedCallback cb) = 0;
    virtual QByteArray read(int maxBytes = 4096) = 0;

    // ── 状态查询 ──
    virtual bool isConnected() const = 0;
    // 返回 const& 而非值：std::atomic 不可拷贝 → ChannelStats 整体不可拷贝；
    // 调用方逐字段 load() 即可（仍是无锁读，无额外开销）。
    virtual const ChannelStats& getStats() const = 0;
    virtual QString lastError() const = 0;

    // ── 回调注册（= 0 强制子类实现；4 个 m_xxxCb 由基类 protected 统一持有） ──
    virtual void setReadCallback(ReadCallback cb) = 0;
    virtual void setWriteCompletedCallback(WriteCompletedCallback cb) = 0;
    virtual void setConnectionChangedCallback(ConnectionChangedCallback cb) = 0;
    virtual void setErrorCallback(ErrorCallback cb) = 0;

protected:
    ReadCallback              m_readCb;
    WriteCompletedCallback    m_writeCb;
    ConnectionChangedCallback m_connCb;
    ErrorCallback             m_errCb;

signals:
    void dataReceived(const QByteArray& data);        // 原始字节到达（IO 线程）
    void writeCompleted(qint64 bytesWritten);          // 物理发送结束（用于 RS485 DE/RE 释放）
    void connectionChanged(bool connected);            // TCP 断/连、串口拔插
    void errorOccurred(const QString& errorMessage);   // IO 错误
};

}  // namespace ens::channel