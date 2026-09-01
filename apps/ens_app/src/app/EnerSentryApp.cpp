// src/app/EnerSentryApp.cpp
// L5 应用层 —— 主程序接线器实现（切片 14：CLI 最小可运行）。
//
// 线程模型落地：
//   * worker 线程内：ModbusEngine + PollScheduler + PollDriver 全同步（无锁）
//   * 主线程：channel 对象 / L1SnapshotStore / DataBus / AlarmEngine
//   * 跨线程边界仅 3 处，全部 Qt::AutoConnection（engine/driver → 主线程 queued）：
//       driver.sampleReady → onSampleReady
//       driver.frameErrorSeen → onFrameErrorSeen（间接打印）
//       channel.connectionChanged → onChannelConnectionChanged
//   * 轮询驱动：QTimer::singleShot 自驱动（static API 跨线程安全），
//     startPolling/stopPolling 经 QMetaObject::invokeMethod 跨线程投递到 worker。

#include "EnerSentryApp.h"

#include <QDateTime>
#include <QMetaObject>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <set>

namespace ens::app {

namespace {

/// 单在途轮询窗口（worker 线程私有，无需加锁）
struct PendingPoll {
    uint8_t  slave     = 0;
    uint16_t startAddr = 0;
    uint16_t qty       = 0;
    bool     active    = false;
};

/// 按寄存器类型选读功能码
uint8_t fcForRead(protocol::RegisterType rt) noexcept {
    switch (rt) {
        case protocol::RegisterType::Coil:           return 0x01;
        case protocol::RegisterType::DiscreteInput:  return 0x02;
        case protocol::RegisterType::InputRegister:  return 0x04;
        case protocol::RegisterType::HoldingRegister:
        default:                                     return 0x03;
    }
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// SampleSink —— DataBus 订阅者桥
// ─────────────────────────────────────────────────────────────────────────────

SampleSink::SampleSink(business::AlarmEngine& alarm, QAtomicInt& count)
    : m_alarm(alarm), m_count(count) {}

void SampleSink::onSample(const datahub::Sample& s) noexcept {
    // 切片 14：AlarmEngine 在主线程事件循环，broadcast 亦在主线程 → 同线程直接调用。
    // onDataUpdated 是 IAlarmEngine 虚函数（非跨线程 marshal），此处直接调。
    m_alarm.onDataUpdated(s.pointId, static_cast<qint64>(s.timestamp), s.value);
    m_count.ref();
}

// ─────────────────────────────────────────────────────────────────────────────
// PollDriver —— worker 线程轮询驱动 + 响应 → Sample 转换
// ─────────────────────────────────────────────────────────────────────────────

class PollDriver : public QObject {
    Q_OBJECT
public:
    /// 一次 FC03 读取的寄存器段（Modbus 上限 125，切片 14 实测坑：qty=332 → sim 回 0x03 异常帧）
    struct Segment {
        uint8_t  slave     = 0;
        uint16_t startAddr = 0;
        uint16_t qty       = 0;
        protocol::RegisterType rt = protocol::RegisterType::HoldingRegister;
    };

    PollDriver(protocol::ModbusEngine* engine, protocol::PollScheduler* sched,
               std::shared_ptr<const protocol::PointTable> pt, SampleBridge* bridge,
               QObject* parent = nullptr)
        : QObject(parent), m_engine(engine), m_sched(sched), m_pt(std::move(pt)),
          m_bridge(bridge) {
        // 轮转从站列表（加载期固化，enabled 过滤）
        for (auto* pr : m_pt->allPoints()) {
            if (pr->enabled) m_slaveList.insert(pr->slaveAddress);
        }
        // TCP 专线全双工：关掉 PollScheduler 半双工 busy 语义（RS485 才需要 onLinkFree 释放）。
        // 否则第一次 dequeueNext 后 busy=true 永不释放 → 后续全部返回空任务（切片 14 实测坑）。
        for (uint8_t sid : m_slaveList) {
            protocol::LinkParams lp;
            lp.isHalfDuplex = false;
            m_sched->setLinkParams(sid, lp);
        }
        // 预构建读取段：每个从站 [minAddr..maxAddr] 按 125 寄存器上限分片。
        // 点表 registerAddr 步进 2（Float32 占 2 寄存器），段起点地址 = base + off*2。
        for (uint8_t sid : m_slaveList) {
            const auto pts = m_pt->allOnSlave(sid);
            if (pts.empty()) continue;
            uint16_t minAddr = pts.front()->registerAddr;
            uint16_t maxAddr = minAddr;
            protocol::RegisterType rt = pts.front()->regType;
            for (const auto* p : pts) {
                minAddr = std::min(minAddr, p->registerAddr);
                maxAddr = std::max(maxAddr, p->registerAddr);
            }
            const uint16_t total = static_cast<uint16_t>((maxAddr - minAddr) / 2 + 1);
            for (uint16_t off = 0; off < total; off += kMaxQtyPerRead) {
                Segment seg;
                seg.slave     = sid;
                seg.startAddr = static_cast<uint16_t>(minAddr + off * 2);
                seg.qty       = std::min<uint16_t>(kMaxQtyPerRead, total - off);
                seg.rt        = rt;
                m_segments.push_back(seg);
            }
        }
    }

public slots:
    /// worker 线程内启动自驱动轮询（经 invokeMethod 跨线程投递）
    void startPolling(int intervalMs) {
        m_intervalMs = intervalMs;
        m_running = true;
        scheduleNext();
    }
    void stopPolling() { m_running = false; }
    void resetPending() { m_pending.active = false; }

    /// 每 tick：取一段 → 入队 → 出队 → 组 FC03 → writeRequest（worker 线程同步）
    void onPollTick() {
        if (!m_running) return;
        if (m_pending.active) { scheduleNext(); return; }   // 单在途：上一帧未响应则跳过
        if (m_segments.empty()) { scheduleNext(); return; }

        const Segment& seg = m_segments[m_nextSegIdx % m_segments.size()];
        ++m_nextSegIdx;

        // 走 PollScheduler 主干（入队 → 优先级出队 → 熔断统计）
        m_sched->enqueue(protocol::PollTask::normal(seg.slave, seg.slave, 1000));
        protocol::PollTask task = m_sched->dequeueNext(seg.slave);
        if (!task.isValid()) { scheduleNext(); return; }

        protocol::ModbusRequest req;
        req.transport       = protocol::Transport::Tcp;
        req.slaveAddress    = seg.slave;
        req.functionCode    = fcForRead(seg.rt);
        req.startingAddress = seg.startAddr;
        req.quantity        = seg.qty;
        const qint64 n = m_engine->writeRequest(req, seg.slave);
        if (n < 0) { scheduleNext(); return; }
        m_pending = PendingPoll{seg.slave, seg.startAddr, seg.qty, true};
        scheduleNext();
    }

    /// worker 线程内：响应 → Sample（PointTable 只读共享，worker 线程安全）
    void onResponseParsed(uint32_t linkId, uint8_t slave,
                          const protocol::ModbusResponse& resp) {
        (void)linkId;
        const bool ok = !resp.isException;
        m_sched->onResponseReceived(slave, ok);
        if (!m_pending.active) return;
        if (slave != m_pending.slave) return;          // 防御：野响应（engine 已按 tid 丢弃）
        m_pending.active = false;
        if (!ok) return;
        if (resp.functionCode != 0x03) return;

        const size_t n = resp.registerValues.size();
        const uint16_t startAddr = m_pending.startAddr;
        for (size_t i = 0; i < n; ) {
            const uint16_t addr = static_cast<uint16_t>(startAddr + i);
            const auto* pr = m_pt->resolve(slave, addr);
            if (!pr) { ++i; continue; }
            // ⚠ 教学版 sim 简化（SIM-IMP §4）：Float32 点按"单寄存器 raw=eng/scale"
            //   编码（encodeFloat32AsHolding），与主程序真 Float32（双寄存器 IEEE754）
            //   语义不一致（切片 14 实测：双寄存器拼 float 全 0）。主程序侧特判对齐；
            //   未来 sim 收口为真 Float32 布局后删除此特判。
            const bool simF32 = (pr->dataType == protocol::DataType::Float32);
            const size_t cnt = simF32 ? 1u
                                      : protocol::PointTable::registerCountFor(pr->dataType);
            if (i + cnt > n) break;
            double v = 0.0;
            if (simF32) {
                v = static_cast<double>(resp.registerValues[i]) * pr->scaleFactor
                    + pr->offset;
            } else {
                uint8_t bytes[8] = {};
                const size_t blen = protocol::PointTable::reassembleBytes(
                    &resp.registerValues[i], cnt, pr->byteOrder, bytes, sizeof(bytes));
                if (blen > 0) {
                    v = m_pt->decodeToEngineering(
                        bytes, blen, pr->dataType, pr->scaleFactor, pr->offset);
                }
            }
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            m_bridge->push(pr->pointId, now, v);   // 跨线程桥：worker 写 → 主线程消费
            i += cnt;
        }
    }

    /// worker 线程内：帧错误 → 熔断计数 + 清在途
    void onFrameError(uint32_t linkId, uint8_t slave, protocol::FrameErrorKind kind) {
        (void)linkId;
        m_sched->onResponseReceived(slave, false);
        m_pending.active = false;
        emit frameErrorSeen(slave, static_cast<int>(kind));
    }

signals:
    void frameErrorSeen(uint8_t slave, int kind);

private:
    void scheduleNext() {
        if (m_running) {
            QTimer::singleShot(m_intervalMs, this, &PollDriver::onPollTick);
        }
    }

    protocol::ModbusEngine*                 m_engine;
    protocol::PollScheduler*                m_sched;
    std::shared_ptr<const protocol::PointTable> m_pt;
    SampleBridge*                           m_bridge;
    std::set<uint8_t>                       m_slaveList;   // 从站集合（构造期用）
    std::vector<Segment>                    m_segments;    // 读取段（≤125 寄存器/段）
    size_t                                  m_nextSegIdx = 0;
    int                                     m_intervalMs = 100;
    bool                                    m_running = false;
    PendingPoll                             m_pending{};
    static constexpr uint16_t               kMaxQtyPerRead = 125;   // Modbus FC03 上限
};

// ─────────────────────────────────────────────────────────────────────────────
// EnerSentryApp
// ─────────────────────────────────────────────────────────────────────────────

EnerSentryApp::EnerSentryApp(const Options& opts, QObject* parent)
    : QObject(parent),
      m_opts(opts),
      // ⚠ m_channel 不带 parent：QObject 带 parent 时 moveToThread 静默无效，
      // socket 无法随 channel 迁到 worker 线程（切片 14 实测坑，与 PollDriver 同）。
      // EnerSentryApp 析构时先 stop()（channel.close + 线程 wait）再析构成员，无泄漏。
      m_channel(nullptr),
      m_alarm(this),
      m_sink(m_alarm, m_sampleCount),
      m_bbx(&m_l1),                 // 切片 16：黑匣子依赖 L1 快照
      m_dal(opts.dataDir),          // 月库根目录（空 = 不落盘）
      m_l2(&m_dal),
      m_engine(&m_channel, protocol::Transport::Tcp) {}

EnerSentryApp::~EnerSentryApp() {
    stop();
}

bool EnerSentryApp::start() {
    if (m_started) return false;

    // 1) 点表（共享只读；worker 读 + 主线程读）
    try {
        // ⚠ 走 filesystem::path 宽字符重载：QString→toStdWString()(UTF-16) →
        //   PointTable::_wfopen 完全绕开 ANSI 转换（中文路径实测坑：
        //   toStdString() 在含中文路径下抛 "No mapping for the Unicode character"）。
        m_pt = protocol::PointTable::loadFromJsonFile(
            std::filesystem::path(m_opts.pointTablePath.toStdWString()));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[ENS] point table load failed: %s\n", e.what());
        return false;
    }

    // 2) L1SnapshotStore 策略初始化（从点表推导，60s 保留窗口）
    {
        QVector<datahub::RingBufferPolicyEntry> policy;
        for (auto* pr : m_pt->allPoints()) {
            if (!pr->enabled) continue;
            datahub::RingBufferPolicyEntry e;
            e.pointId      = pr->pointId;
            e.sampleRateMs = pr->pollIntervalMs;
            e.retentionMs  = 60000;
            e.priority     = pr->priority;
            policy.push_back(e);
        }
        if (!m_l1.initFromPolicy(policy)) {
            std::fprintf(stderr, "[ENS] L1SnapshotStore init failed\n");
            return false;
        }
    }

    // 2.5) 告警规则加载（opts.alarmRulesPath 非空；失败仅告警不阻断运行）
    if (!m_opts.alarmRulesPath.isEmpty()) {
        std::vector<business::AlarmRule> rules;
        std::string err;
        // ⚠ 中文路径走宽字符 path（PointTable::loadFromJsonFile 同策略，绕 ANSI 坑）
        const int n = business::AlarmRuleLoader::loadFromFile(
            std::filesystem::path(m_opts.alarmRulesPath.toStdWString()),
            *m_pt, rules, &err);
        if (n < 0) {
            std::fprintf(stderr, "[ENS] alarm rules load failed: %s\n", err.c_str());
        } else {
            m_alarm.loadRules(rules);
            std::printf("[ENS] alarm rules loaded: %d\n", n);
            if (!err.empty()) {
                std::fprintf(stderr, "[ENS] alarm rules issues: %s\n", err.c_str());
            }
        }
    }

    // 3) worker 线程 + 对象迁移
    m_workerThread.setObjectName("ens-io-worker");
    m_workerThread.start();
    // ⚠ parent 必须为 nullptr：QObject 带 parent 时 moveToThread 静默无效（Qt 文档），
    // 会令 PollDriver 留在主线程 → 与 worker 线程的 engine 跨线程 → responseParsed 走
    // queued 投递（ModbusResponse 未注册 metatype 时静默丢弃，切片 14 实测坑）。
    m_driver = new PollDriver(&m_engine, &m_scheduler, m_pt, &m_bridge,
                              /*parent=*/nullptr);
    m_engine.moveToThread(&m_workerThread);
    m_scheduler.moveToThread(&m_workerThread);
    m_driver->moveToThread(&m_workerThread);
    m_started = true;   // 提前置位：后续任一失败分支的 stop() 能走完整清理路径

    // 4) 接线（moveToThread 后 AutoConnection 按 affinity 自动选 queued/direct）
    m_engine.bindToChannel();                    // dataReceived → onBytesReceived（worker→worker direct）
    connect(&m_engine, &protocol::ModbusEngine::responseParsed,
            m_driver, &PollDriver::onResponseParsed);          // worker→worker direct
    connect(&m_engine, &protocol::ModbusEngine::frameError,
            m_driver, &PollDriver::onFrameError);              // worker→worker direct
    connect(&m_channel, &channel::IChannel::connectionChanged,
            this, &EnerSentryApp::onChannelConnectionChanged); // 主线程 slot
    connect(m_driver, &PollDriver::frameErrorSeen,
            this, [this](uint8_t slave, int kind) {
        std::fprintf(stderr, "[ENS] frame error slave=%u kind=%d\n", slave, kind);
        emit frameErrorSeen(slave, kind);
    });
    connect(&m_alarm, &business::AlarmEngine::alarmTriggered,
            this, [this](const business::AlarmEvent& ev) {
        // 同线程（主线程）direct 调用，AlarmEvent 无需 metatype
        std::printf("[ENS] ALARM pointId=%u level=%d value=%.2f\n",
                    ev.pointId, static_cast<int>(ev.level), ev.alarmValue);
        emit alarmRaised(QStringLiteral("ALARM pid=%1 level=%2")
                         .arg(ev.pointId).arg(static_cast<int>(ev.level)));
    });
    // 切片 16：黑匣子接线（Critical → triggerBlackBox；依赖倒置信号已就绪）
    connect(&m_alarm, &business::AlarmEngine::blackBoxRequested,
            this, [this](uint32_t pid, uint64_t ts, business::AlarmLevel lvl) {
        // ⚠ business::AlarmLevel 与 datahub::AlarmLevel 为不同命名空间同名枚举，
        // 值域一致（Info/Warning/Critical），需显式转换（C2664 实测坑）
        m_bbx.triggerBlackBox(pid, ts, static_cast<datahub::AlarmLevel>(lvl));
    });
    // 切片 16：SBO commandReady → FC06 跨线程下发（worker 线程 ModbusEngine）
    connect(&m_sbo, &business::SboStateMachine::commandReady,
            this, [this](const business::SboCommand& cmd) { dispatchSboCommand(cmd); });
    // 切片 16：SBO 守卫注入（IoC，不持所有权）
    m_sbo.setGuard(&m_sboGuard);
    // 主线程消费节拍：worker 经 SampleBridge 写 Sample → 本槽在消费线程处理
    // （切片 14 实测：worker→主线程 queued 信号在本 CLI 场景投递不可靠，
    //   改确定性轮询，见 SampleBridge 注释）
    connect(&m_consumeTimer, &QTimer::timeout,
            this, &EnerSentryApp::onConsumeTick);
    m_consumeTimer.start(50);

    // 订阅 DataBus（通配）：broadcast → m_sink.onSample → AlarmEngine + 计数
    if (m_bus.subscribeWildcard(&m_sink) == 0) {
        std::fprintf(stderr, "[ENS] DataBus subscribe failed\n");
    }

    // 切片 16：月库周期 flush（dataDir 非空才启动）
    if (!m_opts.dataDir.isEmpty()) {
        connect(&m_flushTimer, &QTimer::timeout, this, &EnerSentryApp::onFlushTick);
        m_flushTimer.start(1000);
    }
    // 切片 16：黑匣子 Critical mmap（blackboxDir 非空；失败降级仅计数）
    if (!m_opts.blackboxDir.isEmpty()) {
        const QString swapPath = m_opts.blackboxDir + QStringLiteral("/critical.swp");
        if (!m_bbx.enableCriticalSwap(swapPath)) {
            std::fprintf(stderr, "[ENS] blackbox critical mmap failed, degraded to counting\n");
        }
    }

    // 5) 通道（异步连接；连接成功回调里启动轮询）
    channel::ChannelConfig cfg;
    cfg.type = channel::ChannelType::TCP;
    channel::TcpConfig tc;
    tc.host = m_opts.host;
    tc.port = m_opts.port;
    cfg.payload = tc;
    cfg.name = QStringLiteral("BMS-LINK-01");
    if (!m_channel.open(cfg)) {
        std::fprintf(stderr, "[ENS] channel open failed: %s\n",
                     qPrintable(m_channel.lastError()));
        stop();
        return false;
    }
    // ⚠ 通道必须与 engine 同线程（切片 14 实测坑）：
    //   QTcpSocket::write 虽标称线程安全，但跨线程调用时数据仅入内部 writeBuffer
    //   排队，flush 依赖 socket 线程事件循环；在无 UI 的 CLI 主循环 + worker 采集
    //   线程组合下实测"返回成功但字节未发"（sim 侧零接收）。
    //   open 之后 moveToThread：socket 作为 TcpChannel 子对象随 parent 迁移，
    //   连接完成事件、readyRead、write 全部落到 worker 线程 → 与 engine 零跨线程。
    m_channel.moveToThread(&m_workerThread);

    // 切片 16：一次性 SBO cmd 注入（等链路连接后执行）
    if (!m_opts.sboCmd.isEmpty()) {
        scheduleSboCmd();
    }

    std::printf("[ENS] started host=%s port=%u pt=%s\n",
                qPrintable(m_opts.host), m_opts.port,
                qPrintable(m_opts.pointTablePath));
    return true;
}

void EnerSentryApp::stop() {
    if (!m_started) return;
    m_started = false;

    m_consumeTimer.stop();
    m_flushTimer.stop();
    if (m_driver) {
        QMetaObject::invokeMethod(m_driver, "stopPolling", Qt::BlockingQueuedConnection);
    }
    // 月库最终 flush（切片 16：dataDir 非空时把剩余降采样结果落盘）
    if (!m_opts.dataDir.isEmpty()) {
        m_l2.flush();
    }
    // 通道在 worker 线程（open 后 moveToThread）→ close 也须在 worker 线程执行
    //（socket 的 abort/deleteLater 非线程安全，跨线程调用 UB）
    QMetaObject::invokeMethod(&m_channel, [this]() { m_channel.close(); },
                              Qt::BlockingQueuedConnection);
    m_connected = false;

    if (m_workerThread.isRunning()) {
        m_workerThread.quit();
        m_workerThread.wait(3000);
    }
    // 线程已结束，可在主线程释放 worker-thread-affinity 对象（Qt 允许：对象不在其线程执行路径上）
    if (m_driver) {
        delete m_driver;
        m_driver = nullptr;
    }
    std::printf("[ENS] stopped (samples=%d)\n", sampleCount());
}

void EnerSentryApp::onChannelConnectionChanged(bool isConnected) {
    m_connected = isConnected;
    // 切片 16：链路状态 → SBO（断开启动 500ms 抖动窗口，防瞬时闪断误清锁）
    m_sbo.onLinkStatusChanged(isConnected);
    if (isConnected) {
        std::printf("[ENS] link connected\n");
        QMetaObject::invokeMethod(m_driver, "startPolling",
                                  Qt::QueuedConnection,
                                  Q_ARG(int, m_opts.pollIntervalMs));
        emit connected();
    } else {
        std::printf("[ENS] link disconnected\n");
        QMetaObject::invokeMethod(m_driver, "stopPolling", Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_driver, "resetPending", Qt::QueuedConnection);
        emit disconnected();
    }
}

void EnerSentryApp::onConsumeTick() {
    uint32_t pid = 0;
    qint64   ts  = 0;
    double   v   = 0.0;
    size_t   n   = 0;
    // 消费桥中全部待处理 Sample（主线程：L1/DataBus/AlarmEngine 同线程安全）
    while (m_bridge.tryPop(&pid, &ts, &v)) {
        ++n;
        datahub::Sample s;
        s.timestamp = static_cast<uint64_t>(ts);
        s.pointId   = pid;
        s.value     = static_cast<float>(v);
        m_l1.write(pid, s);           // 未注册测点静默丢弃（本切片已按点表全量注册）
        m_bus.broadcast(s);           // 同步调 m_sink.onSample → AlarmEngine + 计数
        emit sampleReady(pid, ts, v);
        // 切片 16：降采样 → 月库（dataDir 非空才做；enqueue 有背压保护）
        if (!m_opts.dataDir.isEmpty()) {
            m_ds.feed(pid, s, datahub::HistoryGranularity::Gran1s);
            const auto rolled = m_ds.rollUp(pid, datahub::HistoryGranularity::Gran1s);
            for (const auto& r : rolled) m_l2.enqueueSample(r);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 切片 16：SBO 控制流 / 黑匣子 / 月库
// ─────────────────────────────────────────────────────────────────────────────

void EnerSentryApp::onFlushTick() {
    // 月库周期 flush（dataDir 非空时已由 start() 启动定时器）
    m_l2.flush();
}

bool EnerSentryApp::submitSboSelect(uint32_t slaveId, uint32_t registerAddr,
                                    uint16_t value, bool emergency) {
    business::SboSelectRequest req;
    req.slaveId      = slaveId;
    req.registerAddr = registerAddr;
    req.value        = value;
    req.emergency    = emergency;
    return m_sbo.submitSelect(req, QStringLiteral("cli"));
}

bool EnerSentryApp::submitSboOperate(const QString& sequenceId) {
    return m_sbo.submitOperate(sequenceId.isEmpty() ? m_sbo.currentSequenceId() : sequenceId);
}

bool EnerSentryApp::submitSboCancel(const QString& sequenceId) {
    return m_sbo.submitCancel(sequenceId.isEmpty() ? m_sbo.currentSequenceId() : sequenceId);
}

void EnerSentryApp::dispatchSboCommand(const business::SboCommand& cmd) {
    // 组 FC06 写单寄存器请求；跨线程投递到 worker 线程 ModbusEngine 执行
    protocol::ModbusRequest req;
    req.transport        = protocol::Transport::Tcp;
    req.slaveAddress     = static_cast<uint8_t>(cmd.slaveId);
    req.functionCode     = 0x06;
    req.startingAddress  = static_cast<uint16_t>(cmd.registerAddr);
    req.quantity         = 1;
    req.registerValues   = {static_cast<uint16_t>(cmd.value)};
    QMetaObject::invokeMethod(&m_engine, [this, req]() {
        const qint64 n = m_engine.writeRequest(req, 1);
        std::printf("[ENS] SBO FC06 write slave=%u addr=%u value=%u sent=%lld\n",
                    req.slaveAddress, req.startingAddress, req.registerValues[0],
                    static_cast<long long>(n));
    }, Qt::QueuedConnection);
}

void EnerSentryApp::scheduleSboCmd() {
    // 启动 800ms 后执行（等 TCP 连接 + 轮询就绪）；单次注入后清空
    QTimer::singleShot(800, this, [this]() {
        const QStringList parts = m_opts.sboCmd.split(QLatin1Char(':'));
        if (parts.isEmpty()) return;
        const QString op = parts[0];
        if (op == QLatin1String("select") && parts.size() >= 4) {
            const bool ok = submitSboSelect(parts[1].toUInt(), parts[2].toUInt(),
                                            static_cast<uint16_t>(parts[3].toUInt()),
                                            parts.size() >= 5 && parts[4] == QLatin1String("e"));
            std::printf("[ENS] SBO select submitted=%d seq=%s\n", ok ? 1 : 0,
                        qPrintable(m_sbo.currentSequenceId()));
        } else if (op == QLatin1String("operate")) {
            const bool ok = submitSboOperate();
            std::printf("[ENS] SBO operate submitted=%d\n", ok ? 1 : 0);
        } else if (op == QLatin1String("cancel")) {
            const bool ok = submitSboCancel();
            std::printf("[ENS] SBO cancel submitted=%d\n", ok ? 1 : 0);
        } else {
            std::fprintf(stderr, "[ENS] bad --cmd '%s' (expect select:s:r:v[:e] / operate / cancel)\n",
                         qPrintable(m_opts.sboCmd));
        }
        m_opts.sboCmd.clear();
    });
}

}  // namespace ens::app

#include "EnerSentryApp.moc"
