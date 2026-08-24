// tests/integration/test_tcp_channel.cpp
// Phase 1 L1 2.1.3 Tier 3：TcpChannel 真实收发 + 指数退避重连（ENS-DEV-GUIDE §2A 2.1.3）。
// 进程内起 B3 ModbusTcpServer(127.0.0.1:0) 作对端，TcpChannel 作为客户端走真实 socket 通路。
// 覆盖：连接 + FC03 回环（字节级 + 统计）/ 断线 → 指数退避自动重连 / close 幂等 / 未连接 write=-1。

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <array>
#include <cstdint>
#include <vector>

#include "core/mbap.h"
#include "sim/modbus_tcp_server.h"
#include "TcpChannel.h"

using ens::channel::ChannelConfig;
using ens::channel::ChannelType;
using ens::channel::TcpChannel;
using ens::channel::TcpConfig;
using ens::sim::ModbusTcpServer;

namespace {

// Catch2 进程共享：QCoreApplication 只允许创建一次
QCoreApplication* appInstance() {
    static QCoreApplication* app = [] {
        static int argc = 1;
        static char arg0[] = "ens_tests";
        static char* argv[] = {arg0, nullptr};
        return new QCoreApplication(argc, argv);
    }();
    return app;
}

// 事件循环等待某个 Qt 信号（超时返回 false）；sender 用具体类型（新式 connect 模板推断需要）
template <typename Sender, typename Signal>
class SignalWaiter {
public:
    explicit SignalWaiter(Sender* sender, Signal signal) {
        QObject::connect(sender, signal, &m_loop,
                         [this] { m_signaled = true; m_loop.quit(); });
    }
    bool wait(int timeoutMs) {
        QTimer::singleShot(timeoutMs, &m_loop, &QEventLoop::quit);
        m_loop.exec();
        return m_signaled;
    }

private:
    QEventLoop m_loop;
    bool m_signaled = false;
};

ChannelConfig tcpConfig(const ModbusTcpServer& server) {
    ChannelConfig cfg;
    cfg.type = ChannelType::TCP;
    TcpConfig t;
    t.host = "127.0.0.1";
    t.port = server.actualPort();
    cfg.payload = t;
    return cfg;
}

// 组 FC03 读请求帧（MBAP + PDU），返回完整 wire 帧
std::vector<uint8_t> makeReadFrame(uint16_t tid, uint16_t addr, uint16_t qty) {
    std::vector<uint8_t> frame;
    ens::core::MbapHeader h;
    h.transactionId = tid;
    h.length = 6;                                    // unitId(1) + fc(1) + addr(2) + qty(2)
    h.unitId = 1;
    std::array<uint8_t, 7> mbap{};
    ens::core::emit_mbap(mbap.data(), h);
    frame.insert(frame.end(), mbap.begin(), mbap.end());
    frame.insert(frame.end(), {0x03,
                               static_cast<uint8_t>(addr >> 8), static_cast<uint8_t>(addr & 0xFF),
                               static_cast<uint8_t>(qty >> 8), static_cast<uint8_t>(qty & 0xFF)});
    return frame;
}

}  // namespace

TEST_CASE("tcp_channel: connect to B3 server and round-trip FC03 read", "[channel][tcp]") {
    appInstance();

    ModbusTcpServer server("127.0.0.1", 0);
    REQUIRE(server.open());
    server.regs().set(0, 0x1234);
    server.regs().set(1, 0xABCD);

    TcpChannel ch;
    SignalWaiter conn(&ch, &TcpChannel::connectionChanged);
    REQUIRE(ch.open(tcpConfig(server)));
    REQUIRE(conn.wait(3000));                        // 连接成功（open 为异步 connectToHost）
    REQUIRE(ch.isConnected());

    // FC03 读 addr=0 qty=2 → 期望 PDU [0x03, 0x04, 0x12, 0x34, 0xAB, 0xCD]
    const auto req = makeReadFrame(0x7B2A, 0, 2);
    SignalWaiter rx(&ch, &TcpChannel::dataReceived);
    const int written = ch.write(QByteArray(reinterpret_cast<const char*>(req.data()),
                                            static_cast<int>(req.size())));
    CHECK(written == static_cast<int>(req.size()));
    REQUIRE(rx.wait(3000));                          // 收到响应

    // 用 read() 取缓冲数据并校验（dataReceived 已触发，缓冲里是完整响应帧）
    const QByteArray resp = ch.read(4096);
    REQUIRE(resp.size() == 13);                      // MBAP(7) + PDU(6)
    ens::core::MbapHeader hdr;
    REQUIRE(ens::core::parse_mbap(reinterpret_cast<const uint8_t*>(resp.constData()),
                                  static_cast<size_t>(resp.size()), hdr));
    CHECK(hdr.transactionId == 0x7B2A);              // transactionId 透传
    CHECK(hdr.length == 7);                          // unitId + PDU
    CHECK(static_cast<uint8_t>(resp.at(7)) == 0x03);
    CHECK(static_cast<uint8_t>(resp.at(8)) == 0x04); // byteCount
    CHECK(static_cast<uint8_t>(resp.at(9)) == 0x12);
    CHECK(static_cast<uint8_t>(resp.at(10)) == 0x34);
    CHECK(static_cast<uint8_t>(resp.at(11)) == 0xAB);
    CHECK(static_cast<uint8_t>(resp.at(12)) == 0xCD);

    // 字节统计
    const auto& s = ch.getStats();
    CHECK(s.bytesSent.load() == static_cast<uint64_t>(req.size()));
    CHECK(s.bytesReceived.load() == static_cast<uint64_t>(resp.size()));

    ch.close();
    server.close();
}

TEST_CASE("tcp_channel: disconnect triggers exponential-backoff auto reconnect", "[channel][tcp]") {
    appInstance();

    ModbusTcpServer server("127.0.0.1", 0);
    REQUIRE(server.open());

    TcpChannel ch;
    SignalWaiter conn(&ch, &TcpChannel::connectionChanged);
    REQUIRE(ch.open(tcpConfig(server)));
    REQUIRE(conn.wait(3000));
    REQUIRE(ch.isConnected());

    // server 关闭 → channel 应感知断线（connectionChanged(false)）
    SignalWaiter drop(&ch, &TcpChannel::connectionChanged);
    server.close();
    REQUIRE(drop.wait(3000));
    CHECK_FALSE(ch.isConnected());

    // server 恢复监听同端口 → 指数退避（首次 1s）后自动重连成功
    REQUIRE(server.open());                          // m_port 保留原 actualPort
    SignalWaiter restore(&ch, &TcpChannel::connectionChanged);
    REQUIRE(restore.wait(5000));                     // 1s 退避 + 连接时间（覆盖最坏 2s 二次重试）
    CHECK(ch.isConnected());

    ch.close();
    server.close();
}

TEST_CASE("tcp_channel: close is idempotent", "[channel][tcp]") {
    appInstance();

    ModbusTcpServer server("127.0.0.1", 0);
    REQUIRE(server.open());

    TcpChannel ch;
    SignalWaiter conn(&ch, &TcpChannel::connectionChanged);
    REQUIRE(ch.open(tcpConfig(server)));
    REQUIRE(conn.wait(3000));

    ch.close();
    ch.close();                                      // 二次调用不抛、不重复释放
    CHECK_FALSE(ch.isConnected());

    server.close();
}

TEST_CASE("tcp_channel: write before connect returns -1 with lastError", "[channel][tcp]") {
    appInstance();

    TcpChannel ch;                                   // 未 open，天然未连接
    const int r = ch.write(QByteArray(8, '\x00'));
    CHECK(r == -1);
    CHECK_FALSE(ch.lastError().isEmpty());
}