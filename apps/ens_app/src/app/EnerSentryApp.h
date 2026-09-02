// src/app/EnerSentryApp.h
// L5 应用层 —— 主程序接线器（切片 14：主程序最小可运行，CLI 无 UI）。
//
// 数据流（一次轮询 → 一个 Sample）：
//   [worker] QTimer::singleShot 自驱动 → PollDriver::onPollTick
//       PollScheduler.enqueue → dequeueNext → 组 FC03 请求 → ModbusEngine::writeRequest
//   [worker] channel.dataReceived → engine.onBytesReceived → responseParsed
//       PollDriver::onResponseParsed → PointTable.resolve → decodeToEngineering
//       → emit sampleReady(pointId, ts, value) ──(queued)──► [主线程]
//   [主线程] EnerSentryApp::onSampleReady → L1SnapshotStore.write
//       → DataBus.broadcast → SampleSink::onSample → AlarmEngine.onDataUpdated
//
// 线程模型（与 LLD-100/200/400 一致）：
//   * IO 线程（QTcpSocket 内部）：字节收发
//   * worker 线程：ModbusEngine + PollScheduler + PollDriver（moveToThread）
//   * 主线程：TcpChannel 自身 / L1SnapshotStore / DataBus / AlarmEngine
//
// 不做（切片边界，留 4.3.4 联调）：
//   * SBO 三定时器 / BlackBoxManager 接入 / 告警规则 JSON / 月库落盘
//
// 切片 14 关键实现决策（2026-08-31）：
//   * 轮询驱动用 QTimer::singleShot 自驱动（static 跨线程安全），绕开
//     "QTimer 成员 moveToThread 后 start() 跨线程 UB" 的 Qt 坑。
//   * 单在途语义：onPollTick 若上一帧未响应则跳过（半双工 FIFO 一致），
//     ModbusEngine 的 inFlight tid 配对表本切片仅承载单请求。

#pragma once

#include "TcpChannel.h"
#include "ModbusFrame.h"
#include "ModbusEngine.h"
#include "PollScheduler.h"
#include "PointTable.h"
#include "L1SnapshotStore.h"
#include "DataBus.h"
#include "AlarmEngine.h"
#include "AlarmRecordStore.h"
#include "AlarmRuleLoader.h"
#include "BlackBoxManager.h"
#include "DownSampler.h"
#include "IDataAccess.h"
#include "SQLiteDataAccess.h"
#include "L2HistoryStore.h"
#include "DeviceSboGuard.h"
#include "SboStateMachine.h"

#include <QAtomicInt>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

namespace ens::app {

class PollDriver;   // worker 线程轮询驱动（定义在 EnerSentryApp.cpp，namespace 级而非嵌套类）

// ── 跨线程 Sample 桥：worker 写 / 主线程读（切片 14 实测：Qt queued 信号投递
//    在本 CLI 场景下不可靠，改用 mutex 保护的有界队列，确定性传递）──
class SampleBridge {
public:
    void push(uint32_t pointId, qint64 ts, double value) {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_q.size() >= kMaxPending) m_q.erase(m_q.begin());   // 有界：防消费慢积压
        m_q.emplace_back(pointId, ts, value);
    }
    bool tryPop(uint32_t* pointId, qint64* ts, double* value) {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_q.empty()) return false;
        auto& e = m_q.front();
        *pointId = std::get<0>(e);
        *ts      = std::get<1>(e);
        *value   = std::get<2>(e);
        m_q.erase(m_q.begin());
        return true;
    }
    size_t pendingCount() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_q.size();
    }
private:
    static constexpr size_t kMaxPending = 4096;
    mutable std::mutex m_mtx;
    std::vector<std::tuple<uint32_t, qint64, double>> m_q;
};

// ── DataBus 订阅者桥：broadcast → AlarmEngine + Sample 计数 ──
class SampleSink final : public datahub::IDataBusSubscriber {
public:
    SampleSink(business::AlarmEngine& alarm, QAtomicInt& count);
    void onSample(const datahub::Sample& s) noexcept override;
private:
    business::AlarmEngine& m_alarm;
    QAtomicInt&            m_count;
};

class EnerSentryApp : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString host            = "127.0.0.1";
        quint16 port            = 5020;
        QString pointTablePath;      // 与测试台共享的同一点表 JSON
        int     pollIntervalMs  = 100;
        int     runSeconds      = 0;     // >0: 到时自动 quit（CLI 演示 / 集成测试）
        // 切片 16（4.3.4 Track A 接线）：
        QString alarmRulesPath;      // 告警规则 JSON（缺省不加载规则）
        QString dataDir;             // 月库根目录（缺省禁用落库）
        QString blackboxDir;         // 黑匣子 mmap 目录（缺省禁用 mmap,仅计数）
        QString sboCmd;              // 一次性 SBO 注入："select:slave:reg:value[:e]" / "operate" / "cancel"
    };

    explicit EnerSentryApp(const Options& opts, QObject* parent = nullptr);
    ~EnerSentryApp() override;

    EnerSentryApp(const EnerSentryApp&) = delete;
    EnerSentryApp& operator=(const EnerSentryApp&) = delete;

    /// 构造 + 加载点表 + 接线 + open 通道；失败返 false（诊断已打印 stderr）
    bool start();
    /// 优雅停止：停轮询 → close 通道 → worker 线程 quit+wait
    void stop();

    // ── 诊断 ──
    int  sampleCount() const noexcept { return m_sampleCount.loadAcquire(); }
    bool isConnected() const noexcept { return m_connected; }

    // ── 切片 16：SBO 控制流（Tier 3 集成测试 / CLI --cmd 注入）──
    /// Select：@return true 状态机接受
    bool submitSboSelect(uint32_t slaveId, uint32_t registerAddr, uint16_t value,
                         bool emergency = false);
    /// Operate 二次确认：seq 空则用当前 sequenceId
    bool submitSboOperate(const QString& sequenceId = QString());
    bool submitSboCancel(const QString& sequenceId = QString());
    QString currentSboSequenceId() const { return m_sbo.currentSequenceId(); }
    business::SBOState sboState() const { return m_sbo.currentState(); }

    // ── 切片 16：黑匣子 / 月库诊断 ──
    uint32_t blackboxTriggerCount() const { return m_bbx.criticalTriggerCount(); }
    size_t   historyPendingCount() const { return m_l2.pendingCount(); }
    uint32_t alarmCount() const { return m_alarm.activeAlarmCount(); }

    // ── 切片 19：UI 依赖注入访问器（只读；ens::ui 层经此拿数据源，不依赖 app 层头）──
    datahub::DataBus*      dataBus() noexcept      { return &m_bus; }
    business::AlarmEngine* alarmEngine() noexcept  { return &m_alarm; }
    // 切片 21：SBO 状态机（UI 绑定 sboStateChanged；下发仍经 submitSboXxx 公开方法）
    business::SboStateMachine* sboStateMachine() noexcept { return &m_sbo; }
    // 切片 23：Diag/Config 视图数据源（只读；ens::ui 不触碰 app 层头）
    channel::TcpChannel* channel() noexcept { return &m_channel; }
    const std::shared_ptr<protocol::PointTable>& pointTable() const noexcept { return m_pt; }
    QString alarmRulesPath() const { return m_opts.alarmRulesPath; }
    // 切片 24：历史查询抽象（HistoryTrendWidget 注入；m_dal 未启用时仍可注入，查询返空）
    datahub::IDataAccess* dataAccess() noexcept { return &m_dal; }

signals:
    void connected();
    void disconnected();
    void sampleReady(uint32_t pointId, qint64 timestamp, double value);
    void frameErrorSeen(uint8_t slave, int kind);
    void alarmRaised(const QString& text);   // 切片 14 仅 console 演示

private slots:
    void onChannelConnectionChanged(bool isConnected);
    void onConsumeTick();   // 主线程节拍：从 SampleBridge 取 Sample → L1/DataBus/AlarmEngine
    void onFlushTick();     // 切片 16：月库周期 flush（1s）

private:
    /// 解析 --sbo-cmd 并一次性注入（start 后 800ms，等链路连接）
    void scheduleSboCmd();
    /// SboCommand → FC06 ModbusRequest 跨线程下发
    void dispatchSboCommand(const business::SboCommand& cmd);

    Options                  m_opts;
    bool                     m_started  = false;
    bool                     m_connected = false;
    QAtomicInt               m_sampleCount{0};
    SampleBridge             m_bridge;        // worker 写 → 主线程消费

    // ── 主线程对象（声明序 = 依赖序：m_l1 先于 m_bbx, m_dal 先于 m_l2）──
    channel::TcpChannel      m_channel;
    business::AlarmEngine    m_alarm;
    datahub::L1SnapshotStore m_l1;
    datahub::DataBus         m_bus;
    SampleSink               m_sink;
    datahub::BlackBoxManager m_bbx;          // &m_l1
    datahub::SQLiteDataAccess m_dal;         // dataRootDir = opts.dataDir
    datahub::DownSampler     m_ds;
    datahub::L2HistoryStore  m_l2;           // &m_dal
    business::DeviceSboGuard m_sboGuard;
    business::SboStateMachine m_sbo;         // setGuard(&m_sboGuard) 在 start()
    // 切片 35：告警记录持久化（&m_alarm + &m_dal；dataDir 空 → 自动禁用）
    business::AlarmRecordStore m_alarmStore;
    QTimer                   m_consumeTimer;  // 主线程 50ms 消费节拍
    QTimer                   m_flushTimer;    // 主线程 1s 月库 flush

    // ── worker 线程对象（start() 时 moveToThread；声明序保证构造依赖）──
    QThread                  m_workerThread;
    protocol::ModbusEngine   m_engine;
    protocol::PollScheduler  m_scheduler;
    PollDriver*              m_driver = nullptr;   // start() 里 new，stop() 里 delete

    std::shared_ptr<protocol::PointTable> m_pt;    // 共享只读（worker 读 + 主线程读）
};

}  // namespace ens::app
