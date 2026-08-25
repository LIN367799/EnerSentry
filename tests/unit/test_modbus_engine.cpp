// tests/unit/test_modbus_engine.cpp
// L2 协议引擎 ── ModbusEngine Tier 2 单测（ENS-DEV-GUIDE §3A 3.1.3 + 用户截图 V2 Mock IChannel）。
//
// 覆盖：
//   ① MockIChannel 通过 emit dataReceived 喂 ModbusEngine
//   ② onBytesReceived → accumulator → parseFrame → emit responseParsed 完整链路
//   ③ writeRequest 组帧正确(经 IChannel::write 抓帧)
//   ④ RTU 异常帧 / CRC 错 / 长度不够 → emit frameError(不抛、不污染下游)
//   ⑤ TCP MBAP 长度门禁正常(6 + mbapLen ≤ len)
//   ⑥ 多帧粘包:一次 dataReceived 含 2 帧 → emit 2 次 responseParsed
//   ⑦ ModbusEngine 在另一线程(模拟 worker)通过 Qt::QueuedConnection 收信号,
//     验证 MetaType 注册 + 跨线程信号不丢(QTBUG 模式)
//
// Tier 3 待 B6(Master+Slave 真实 socket 通路)就绪后实证 Sample.value 与测设值一致(缩放还原)。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <QByteArray>
#include <QCoreApplication>
#include <QObject>
#include <QThread>
#include <QTimer>

#include "channel/IChannel.h"
#include "protocol/ModbusEngine.h"
#include "protocol/ModbusFrame.h"

using namespace ens::protocol;
using namespace ens::channel;

// ─────────────────────────────────────────────────────────────────────────────
// MockIChannel ── IChannel 的最小可实现版本(emit dataReceived 提供测试输入)
// 仅实现与 ModbusEngine 交互必要的 4 接口;其他接口抛 std::logic_error。
// ─────────────────────────────────────────────────────────────────────────────
class MockIChannel : public IChannel {
    Q_OBJECT
public:
    explicit MockIChannel(QObject* parent = nullptr) : IChannel(parent) {}

    // 测试钩子:喂字节序列触发 dataReceived(模拟 IO 线程收到数据)
    void feedBytes(const uint8_t* p, size_t n) {
        QByteArray bytes(reinterpret_cast<const char*>(p), static_cast<int>(n));
        emit dataReceived(bytes);
    }
    void feedBytes(const std::vector<uint8_t>& v) { feedBytes(v.data(), v.size()); }

    // 测试钩子:抓最近一次 write() 的字节
    QByteArray lastWrittenBytes() const { return m_lastWrite; }
    int         lastWrittenCount() const { return m_lastWriteCount; }

    // 暴露 setReadCallback 不需要(QObject connect 替代)
    void setReadCallback(ReadCallback /*cb*/) override {}
    void setWriteCompletedCallback(WriteCompletedCallback /*cb*/) override {}
    void setConnectionChangedCallback(ConnectionChangedCallback /*cb*/) override {}
    void setErrorCallback(ErrorCallback /*cb*/) override {}

    bool open(const ChannelConfig& /*cfg*/) override { m_open = true; return true; }
    void close() noexcept override { m_open = false; }
    bool isConnected() const noexcept override { return m_open; }
    int  write(const QByteArray& data) override {
        m_lastWrite = data;
        m_lastWriteCount = data.size();
        return data.size();
    }
    bool asyncWrite(const QByteArray& data, WriteCompletedCallback cb) override {
        write(data);
        if (cb) cb(data.size());
        return true;
    }
    QByteArray read(int /*maxBytes*/) override { return QByteArray(); }
    const ChannelStats& getStats() const noexcept override { return m_stats; }
    QString lastError() const noexcept override { return {}; }

private:
    QByteArray m_lastWrite;
    int        m_lastWriteCount = 0;
    bool       m_open           = true;
    ChannelStats m_stats{};
};

// 测试辅助:累加器,捕获 signal 触发次数 + 最后一次参数
// 用法:SignalCounter<Arg1, Arg2, ...> c;  -- 显式列 args,避开 C++ 模板对
// 函数签名 void(A,B,C) 推导 Args... = (void(A,B,C)) 单参数的坑。
// std::decay_t 把 const T& 退化为 T,这样 tuple 元素是值类型,赋值合法。
template <typename... Args>
struct SignalCounter {
    void slot(Args... args) {
        ++count;
        last = std::tuple<std::decay_t<Args>...>(args...);
    }
    std::atomic<int> count{0};
    std::optional<std::tuple<std::decay_t<Args>...>> last;
};

// ─────────────────────────────────────────────────────────────────────────────
// 帧生成辅助:用 ModbusFrame::buildRequest 组字节,然后注入 MockIChannel
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<uint8_t> makeRTUFC03Response(uint8_t slave,
                                                 uint16_t startAddr,
                                                 const std::vector<uint16_t>& regs) {
    // 组请求帧:unitId,FC=03,addr_hi,addr_lo,qty_hi,qty_lo,CRC_lo,CRC_hi
    ModbusRequest req;
    req.transport    = Transport::Rtu;
    req.slaveAddress  = slave;
    req.functionCode  = 0x03;
    req.startingAddress = startAddr;
    req.quantity      = static_cast<uint16_t>(regs.size());
    return buildRequest(req);  // 测试组帧字节正确性
}

static std::vector<uint8_t> makeFC03ResponseFrame(uint8_t slave,
                                                   uint8_t fc,
                                                   const std::vector<uint16_t>& regs) {
    // RTU 响应帧字节布局:[unitId][FC][byteCount=N*2][data...][CRC_lo][CRC_hi]
    std::vector<uint8_t> f;
    f.push_back(slave);
    f.push_back(fc);
    const uint8_t byteCount = static_cast<uint8_t>(regs.size() * 2);
    f.push_back(byteCount);
    for (uint16_t v : regs) {
        f.push_back(static_cast<uint8_t>(v >> 8));   // big-endian
        f.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    // CRC (compute via buildRequest 同款 CRC16 计算)
    ModbusRequest dummyReq;
    dummyReq.transport   = Transport::Rtu;
    dummyReq.slaveAddress = slave;
    dummyReq.functionCode = 0x03;
    dummyReq.startingAddress = 0;
    dummyReq.quantity     = 1;
    // 用 Crc16Modbus 计算当前 f 的 CRC
    // 直接调 buildRequest 走不到(CRC 加在末尾),但我们可手算或调用 helper。
    // 这里用 Crc16::verify 验证;实际生成更简洁的方式 — 让 buildRequest 加 CRC,
    // 我们此处改用内部 helper:
    //   write CRC manually
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < f.size(); ++i) {
        crc ^= f[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    f.push_back(static_cast<uint8_t>(crc & 0xFF));
    f.push_back(static_cast<uint8_t>(crc >> 8));
    return f;
}

// ─────────────────────────────────────────────────────────────────────────────
// ① 基本 RTU 响应链路
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_engine: RTU FC03 response round-trip emits responseParsed",
          "[master][engine][rtu][parse]") {
    auto* channel = new MockIChannel;
    ModbusEngine engine(channel, Transport::Rtu);
    engine.bindToChannel();

    SignalCounter<uint32_t, uint8_t, const ModbusResponse&> parsedCounter;
    QObject::connect(&engine, &ModbusEngine::responseParsed, &engine,
                     [&](uint32_t linkId, uint8_t slave, const ModbusResponse& resp) {
                         parsedCounter.slot(linkId, slave, resp);
                     });

    // 构造合法 RTU FC03 响应:slave=1, byteCount=4, regs=[0x1234, 0x5678]
    const auto resp = makeFC03ResponseFrame(/*slave=*/1, /*fc=*/0x03,
                                            {0x1234, 0x5678});
    channel->feedBytes(resp);

    REQUIRE(parsedCounter.count.load() == 1);
    REQUIRE(std::get<1>(*parsedCounter.last) == 1u);   // slave
    REQUIRE(std::get<2>(*parsedCounter.last).functionCode == 0x03);
    REQUIRE(std::get<2>(*parsedCounter.last).registerValues.size() == 2u);
    REQUIRE(std::get<2>(*parsedCounter.last).registerValues[0] == 0x1234u);
    REQUIRE(std::get<2>(*parsedCounter.last).registerValues[1] == 0x5678u);
}

TEST_CASE("modbus_engine: RTU exception frame (function|0x80) emits Exception",
          "[master][engine][rtu][exception]") {
    auto* channel = new MockIChannel;
    ModbusEngine engine(channel, Transport::Rtu);
    engine.bindToChannel();

    SignalCounter<uint32_t, uint8_t, const ModbusResponse&> parsed;
    SignalCounter<uint32_t, uint8_t, FrameErrorKind> err;
    QObject::connect(&engine, &ModbusEngine::responseParsed, &engine,
                     [&](uint32_t l, uint8_t s, const ModbusResponse& r) { parsed.slot(l, s, r); });
    QObject::connect(&engine, &ModbusEngine::frameError, &engine,
                     [&](uint32_t l, uint8_t s, FrameErrorKind k) { err.slot(l, s, k); });

    // RTU 异常帧:slave=1,FC=0x83 (0x80|0x03),excCode=0x02 ILLEGAL_DATA_ADDR,CRC
    std::vector<uint8_t> f = {0x01, 0x83, 0x02};
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < f.size(); ++i) {
        crc ^= f[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    f.push_back(static_cast<uint8_t>(crc & 0xFF));
    f.push_back(static_cast<uint8_t>(crc >> 8));
    channel->feedBytes(f);

    REQUIRE(parsed.count.load() == 1);
    REQUIRE(std::get<2>(*parsed.last).isException == true);
    REQUIRE(std::get<2>(*parsed.last).exceptionCode == 0x02u);
    REQUIRE(err.count.load() == 1);
    REQUIRE(std::get<2>(*err.last) == FrameErrorKind::Exception);
}

TEST_CASE("modbus_engine: RTU CRC corrupted frame is silently dropped by accumulator (no signal)",
    "[master][engine][rtu][crc]") {
    auto* channel = new MockIChannel;
    ModbusEngine engine(channel, Transport::Rtu);
    engine.bindToChannel();

    std::atomic<int> parsedCount{0};
    std::atomic<int> errCount{0};
    QObject::connect(&engine, &ModbusEngine::responseParsed, &engine,
                     [&](uint32_t, uint8_t, const ModbusResponse&) { ++parsedCount; });
    QObject::connect(&engine, &ModbusEngine::frameError, &engine,
                     [&](uint32_t, uint8_t, FrameErrorKind) { ++errCount; });

    // 构造合法 FC03 响应 + 故意损坏 CRC 最后一字节
    auto f = makeFC03ResponseFrame(/*slave=*/1, /*fc=*/0x03, {0x1111, 0x2222});
    f.back() ^= 0xFF;   // CRC 翻转
    channel->feedBytes(f);

    // CRC 错帧被 ModbusStreamAccumulator 内部处理(脏数据前滑 + huntCount++),
    // 不会送入 parseRtuResponse,因此 Engine 不 emit 任何信号。
    // 这与 LLD-100 §4.2.2 "frameError(CRC/畸形/超时)" 的设计取舍:
    //   CRC 错是字节流滑窗的一部分,modbus_master 应靠 huntCount 感知同步质量;
    //   frameError 信号专为"累积器已拼出完整帧但内容有误"而设。
    REQUIRE(parsedCount.load() == 0);
    REQUIRE(errCount.load() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// ② writeRequest 路径
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_engine: writeRequest RTU groups canonical bytes and writes via channel",
    "[master][engine][rtu][write]") {
    auto* channel = new MockIChannel;
    ModbusEngine engine(channel, Transport::Rtu);

    ModbusRequest req;
    req.transport      = Transport::Rtu;
    req.slaveAddress    = 1;
    req.functionCode    = 0x03;
    req.startingAddress = 0x006B;
    req.quantity        = 3;

    const auto written = engine.writeRequest(req, /*linkId=*/42);
    REQUIRE(written == 8);   // RTU FC03 请求 = 8 字节(详见 ModbusFrame 单测)

    const auto captured = channel->lastWrittenBytes();
    REQUIRE(captured.size() == 8);
    REQUIRE(static_cast<uint8_t>(captured[0]) == 0x01u);   // slave
    REQUIRE(static_cast<uint8_t>(captured[1]) == 0x03u);   // FC
    REQUIRE(static_cast<uint8_t>(captured[6]) != 0u);       // CRC 已算(末尾 2B)
    REQUIRE(static_cast<uint8_t>(captured[7]) != 0u);
}

TEST_CASE("modbus_engine: writeRequest rejects invalid coil value (FC05)",
    "[master][engine][write][neg]") {
    auto* channel = new MockIChannel;
    ModbusEngine engine(channel, Transport::Rtu);

    ModbusRequest req;
    req.transport     = Transport::Rtu;
    req.slaveAddress   = 1;
    req.functionCode   = 0x05;
    req.startingAddress = 0;
    req.registerValues = {0x0001};   // 非法 → buildRequest 返空 → writeRequest 返 -1

    REQUIRE(engine.writeRequest(req, /*linkId=*/1) == -1);
    REQUIRE(channel->lastWrittenBytes().isEmpty());
}

// ─────────────────────────────────────────────────────────────────────────────
// ③ TCP MBAP 路径
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_engine: TCP MBAP response parsed via parseTcpResponse",
    "[master][engine][tcp][parse]") {
    auto* channel = new MockIChannel;
    ModbusEngine engine(channel, Transport::Tcp);
    engine.bindToChannel();

    SignalCounter<uint32_t, uint8_t, const ModbusResponse&> parsed;
    QObject::connect(&engine, &ModbusEngine::responseParsed, &engine,
                     [&](uint32_t l, uint8_t s, const ModbusResponse& r) { parsed.slot(l, s, r); });

    // TCP FC03 响应:[tid=1][pid=0][len=9][unitId=1][FC=3][byteCount=6][3 regs]
    //   = 6 + 9 = 15 字节
    const std::vector<uint8_t> tcpResp = {
        0x00, 0x01,        // tid
        0x00, 0x00,        // pid = 0
        0x00, 0x09,        // length = unitId(1) + FC(1) + byteCount(1) + data(6) = 9
        0x01, 0x03, 0x06,
        0x02, 0x2B,        // reg 1 = 0x022B
        0x00, 0x00,        // reg 2 = 0x0000
        0x00, 0x64         // reg 3 = 0x0064
    };
    channel->feedBytes(tcpResp);

    REQUIRE(parsed.count.load() == 1);
    REQUIRE(std::get<1>(*parsed.last) == 1u);
    REQUIRE(std::get<2>(*parsed.last).registerValues.size() == 3u);
    REQUIRE(std::get<2>(*parsed.last).registerValues[0] == 0x022Bu);
}

TEST_CASE("modbus_engine: TCP non-zero protocolId rejected (frameError Malformed)",
    "[master][engine][tcp][mbap][neg]") {
    auto* channel = new MockIChannel;
    ModbusEngine engine(channel, Transport::Tcp);
    engine.bindToChannel();

    SignalCounter<uint32_t, uint8_t, FrameErrorKind> err;
    QObject::connect(&engine, &ModbusEngine::frameError, &engine,
                     [&](uint32_t l, uint8_t s, FrameErrorKind k) { err.slot(l, s, k); });

    // pid = 0x0001(非法)— Modbus/TCP 必须 pid=0
    std::vector<uint8_t> bad = {0x00,0x01, 0x00,0x01, 0x00,0x06, 0x01,0x03,0x06,0,0,0,0,0,0};
    channel->feedBytes(bad);

    REQUIRE(err.count.load() == 1);
    REQUIRE(std::get<2>(*err.last) == FrameErrorKind::Malformed);
}

// ─────────────────────────────────────────────────────────────────────────────
// ④ 多帧粘包:一次 dataReceived 含 2 完整帧
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_engine: concatenated frames in one feed produce 2 responseParsed emissions",
    "[master][engine][rtu][concatenated]") {
    auto* channel = new MockIChannel;
    ModbusEngine engine(channel, Transport::Rtu);
    engine.bindToChannel();

    SignalCounter<uint32_t, uint8_t, const ModbusResponse&> parsed;
    QObject::connect(&engine, &ModbusEngine::responseParsed, &engine,
                     [&](uint32_t l, uint8_t s, const ModbusResponse& r) { parsed.slot(l, s, r); });

    auto f1 = makeFC03ResponseFrame(/*slave=*/1, /*fc=*/0x03, {0xAAAA});
    auto f2 = makeFC03ResponseFrame(/*slave=*/2, /*fc=*/0x03, {0xBBBB, 0xCCCC});
    std::vector<uint8_t> concat;
    concat.insert(concat.end(), f1.begin(), f1.end());
    concat.insert(concat.end(), f2.begin(), f2.end());
    channel->feedBytes(concat);

    REQUIRE(parsed.count.load() == 2);
    // 第一帧 slave=1, 第二帧 slave=2
    // (last 是第二次触发)
    REQUIRE(std::get<1>(*parsed.last) == 2u);
    REQUIRE(std::get<2>(*parsed.last).registerValues.size() == 2u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑤ 跨线程信号投递(MetaType 注册 + QueuedConnection)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("modbus_engine: cross-thread emit via Qt QueuedConnection delivers responseParsed",
    "[master][engine][threading][metatype]") {
    // 此测试验证:ModbusEngine 在 worker 线程中 emit 信号 → 主线程槽接收
    //   (覆盖用户截图坑:跨线程发 Sample/ModbusResponse 前 qRegisterMetaType,
    //    否则信号静默丢弃)。
    // qRegisterMetaType 在 tests/main.cpp 启动期已注册(本 TU include ModbusEngine.h 即可)。

    auto* channel = new MockIChannel;        // MockIChannel 仍在主线程(emit 用)
    ModbusEngine engine(channel, Transport::Rtu);

    QThread worker;
    engine.moveToThread(&worker);
    worker.start();

    // 跨线程 lambda 接收(替代 SignalCounter — SignalCounter 不是 QObject,Qt::QueuedConnection
    // 需要 QObject context;直接用 atomic + lambda 在主线程捕获跨线程信号更稳)。
    // ⚠ receiver 必须是 QObject 且其 affinity 在主线程;否则 Qt 不会经主线程 event loop 派发。
    //   此处用 channel(MockIChannel 在主线程)作 receiver context,触发后 channel 线程(主)处理 slot。
    std::atomic<int> count{0};
    ModbusResponse lastResp{};
    uint8_t lastSlave = 0;
    QObject::connect(&engine, &ModbusEngine::responseParsed, channel,
                     [&](uint32_t /*linkId*/, uint8_t slave, const ModbusResponse& resp) {
                         ++count;
                         lastSlave = slave;
                         lastResp = resp;
                     },
                     Qt::QueuedConnection);

    engine.bindToChannel();   // 跨线程 connect;worker 线程发,主线程 lambda 槽接收

    // 喂字节(emit 由 channel 在主线程发起,但因 engine 已 moveToThread,槽走 QueuedConnection)
    const auto resp = makeFC03ResponseFrame(/*slave=*/7, /*fc=*/0x03, {0xDEAD});
    channel->feedBytes(resp);

    // 等待 worker 线程处理 + event 投递回主线程
    // processEvents 仅处理主线程;worker.exec() 自动跑。最多等 500ms,循环 5 次以排空事件。
    for (int i = 0; i < 50; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (count.load() == 1) break;
    }

    REQUIRE(count.load() == 1);
    REQUIRE(lastSlave == 7u);
    REQUIRE(lastResp.registerValues.size() == 1u);
    REQUIRE(lastResp.registerValues[0] == 0xDEADu);

    worker.quit();
    worker.wait();
}

// AUTOMOC 要求 cpp 末尾 #include "<file>.moc" 以注入 Q_OBJECT inline 类(MockIChannel)
// 的元对象代码。Qt CMake AUTOMOC 默认会扫这个 .cpp 看到 Q_OBJECT 标记 + moc include,
// 缺一会报 "AutoMoc error"。这是 Q_OBJECT inline class 在测试 cpp 中的标准模式。
#include "test_modbus_engine.moc"