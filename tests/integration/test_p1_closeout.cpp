// tests/integration/test_p1_closeout.cpp
// Phase 1 收口联调（ENS-DEV-GUIDE §2A 2.1.6 + §2C）：把已落地的四组件
// （TcpChannel 2.1.3 / ModbusStreamAccumulator 2.1.4 / TransactionIdAllocator 2.1.5 / B3 server）
// 组合成真实 socket 数据通路，验证：粘包切分、拆包重组、事务 ID 边界配对。
// 通过标准（§2C）：收发计数相等、crc=ok 100%、响应 tid 与请求一一配对。

#include <catch2/catch_test_macros.hpp>

#include "common/test_helpers.h"

#include <QByteArray>
#include <QDeadlineTimer>

#include <array>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "TcpChannel.h"
#include "ModbusStreamAccumulator.h"
#include "TransactionIdAllocator.h"
#include "sim/modbus_tcp_server.h"

using ens::channel::ChannelConfig;
using ens::channel::ChannelType;
using ens::channel::TcpChannel;
using ens::channel::TcpConfig;
using ens::protocol::ModbusStreamAccumulator;
using ens::protocol::TransactionIdAllocator;
using ens::sim::ModbusTcpServer;
using ens::test::SignalWaiter;
using ens::test::appInstance;
using ens::test::makeReadFrame;

namespace {

ChannelConfig tcpConfig(const ModbusTcpServer& server) {
    ChannelConfig cfg;
    cfg.type = ChannelType::TCP;
    TcpConfig t;
    t.host = "127.0.0.1";
    t.port = server.actualPort();
    cfg.payload = t;
    return cfg;
}

// 阻塞式收集 channel 接收流，直到累加器能提出 expectFrames 帧或超时；
// 每次提帧调用 onFrame(tid, 帧字节)。
template <typename Fn>
int collectFrames(TcpChannel& ch, ModbusStreamAccumulator& acc,
                  int expectFrames, int timeoutMs, Fn onFrame) {
    const QDeadlineTimer deadline(timeoutMs);
    std::array<uint8_t, 4096> out{};
    size_t outLen = out.size();                   // in/out：容量 → 实际帧长
    int extracted = 0;
    while (extracted < expectFrames && !deadline.hasExpired()) {
        while (acc.tryExtractFrame(out.data(), outLen, /*isTcp=*/true)) {
            const std::vector<uint8_t> frame(out.data(), out.data() + outLen);
            ens::core::MbapHeader h;
            (void)ens::core::parse_mbap(frame.data(), frame.size(), h);
            onFrame(h.transactionId, frame);
            ++extracted;
            outLen = out.size();                  // 重置容量
        }
        if (extracted >= expectFrames) break;
        SignalWaiter rx(&ch, &TcpChannel::dataReceived);
        if (!rx.wait(500)) break;                 // 500ms 无新数据则停止
        const QByteArray d = ch.read(4096);
        acc.append(reinterpret_cast<const uint8_t*>(d.constData()),
                   static_cast<size_t>(d.size()));
        outLen = out.size();
    }
    return extracted;
}

}  // namespace

TEST_CASE("p1_closeout: 3 concatenated requests in one write -> 3 extracted responses", "[p1][tcp]") {
    appInstance();

    ModbusTcpServer server("127.0.0.1", 0);
    REQUIRE(server.open());
    server.regs().set(0, 0x1357);

    TcpChannel ch;
    SignalWaiter conn(&ch, &TcpChannel::connectionChanged);
    REQUIRE(ch.open(tcpConfig(server)));
    REQUIRE(conn.wait(3000));
    REQUIRE(ch.isConnected());

    // 3 帧拼接成一段字节流一次写入（模拟粘包）
    const auto f1 = makeReadFrame(0x1001, 0, 1);
    const auto f2 = makeReadFrame(0x1002, 0, 1);
    const auto f3 = makeReadFrame(0x1003, 0, 1);
    QByteArray burst;
    burst.append(reinterpret_cast<const char*>(f1.data()), static_cast<int>(f1.size()));
    burst.append(reinterpret_cast<const char*>(f2.data()), static_cast<int>(f2.size()));
    burst.append(reinterpret_cast<const char*>(f3.data()), static_cast<int>(f3.size()));

    const int written = ch.write(burst);
    CHECK(written == burst.size());               // 三帧全部入队（字节 I/O 对称）

    ModbusStreamAccumulator acc;
    std::vector<uint16_t> gotTids;
    std::vector<size_t> gotLens;
    const int frames = collectFrames(ch, acc, 3, 3000,
        [&](uint16_t tid, const std::vector<uint8_t>& frame) {
            gotTids.push_back(tid);
            gotLens.push_back(frame.size());
        });
    REQUIRE(frames == 3);                         // 粘包切分计数 = 3
    CHECK(gotTids == std::vector<uint16_t>({0x1001, 0x1002, 0x1003}));   // tid 原样透传
    for (const size_t len : gotLens) CHECK(len == 11);                   // MBAP7 + PDU4（fc+byteCount+2B）

    // 收发字节相等（§2C TCP 字节 I/O 行）
    const auto& s = ch.getStats();
    CHECK(s.bytesSent.load() == static_cast<uint64_t>(burst.size()));
    CHECK(s.bytesReceived.load() == static_cast<uint64_t>(11 * 3));

    ch.close();
    server.close();
}

TEST_CASE("p1_closeout: single response fragmented into 5 chunks reassembled intact", "[p1][tcp]") {
    appInstance();

    ModbusTcpServer server("127.0.0.1", 0);
    REQUIRE(server.open());
    // 8 个寄存器 → 响应 PDU = fc+byteCount+16 = 18 字节 → 整帧 25 字节
    for (int i = 0; i < 8; ++i) server.regs().set(static_cast<size_t>(i), static_cast<uint16_t>(0x1000 + i));

    TcpChannel ch;
    SignalWaiter conn(&ch, &TcpChannel::connectionChanged);
    REQUIRE(ch.open(tcpConfig(server)));
    REQUIRE(conn.wait(3000));

    const auto req = makeReadFrame(0x5566, 0, 8);
    REQUIRE(ch.write(QByteArray(reinterpret_cast<const char*>(req.data()),
                                static_cast<int>(req.size()))) == static_cast<int>(req.size()));

    // 收集完整响应帧字节
    QDeadlineTimer deadline(3000);
    QByteArray full;
    while (full.size() < 25 && !deadline.hasExpired()) {
        SignalWaiter rx(&ch, &TcpChannel::dataReceived);
        if (!rx.wait(500)) break;
        full.append(ch.read(4096));
    }
    REQUIRE(full.size() == 25);                   // 整帧 25 字节

    // 模拟 TCP 拆包：25 字节切 5 段（5,5,5,5,5）依次喂累加器 → 重组出完整 1 帧
    ModbusStreamAccumulator acc;
    for (int seg = 0; seg < 5; ++seg) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(full.constData()) + seg * 5;
        acc.append(p, 5);
    }

    std::array<uint8_t, 4096> out{};
    size_t outLen = out.size();                   // in/out：容量 → 实际帧长
    REQUIRE(acc.tryExtractFrame(out.data(), outLen, /*isTcp=*/true));
    REQUIRE(outLen == static_cast<size_t>(full.size()));
    CHECK(std::memcmp(out.data(), full.constData(), outLen) == 0);        // 重组字节级一致

    ens::core::MbapHeader h;
    REQUIRE(ens::core::parse_mbap(out.data(), outLen, h));
    CHECK(h.transactionId == 0x5566);             // tid 透传
    CHECK(h.length == 19);                        // unitId + PDU(18)
    CHECK(out[7] == 0x03);                        // fc
    CHECK(out[8] == 16);                          // byteCount
    CHECK(out[9] == 0x10);                        // regs[0]=0x1000 大端
    CHECK(out[10] == 0x00);
    CHECK(out[23] == 0x10);                       // regs[7]=0x1007 大端
    CHECK(out[24] == 0x07);

    ch.close();
    server.close();
}

TEST_CASE("p1_closeout: 100 txid-allocated requests round-trip with distinct pairing", "[p1][tcp][txid]") {
    appInstance();

    ModbusTcpServer server("127.0.0.1", 0);
    REQUIRE(server.open());
    for (int i = 0; i < 64; ++i) server.regs().set(static_cast<size_t>(i), static_cast<uint16_t>(0x2000 + i));

    TcpChannel ch;
    SignalWaiter conn(&ch, &TcpChannel::connectionChanged);
    REQUIRE(ch.open(tcpConfig(server)));
    REQUIRE(conn.wait(3000));

    // 分配 100 个事务 ID 并一次性发出（粘包 + 事务边界的组合验证）
    TransactionIdAllocator alloc;
    std::vector<uint16_t> tids;
    tids.reserve(100);
    QByteArray burst;
    for (int i = 0; i < 100; ++i) {
        const uint16_t tid = alloc.allocate();
        REQUIRE(tid != 0);                        // 100 << 65535，必成功
        tids.push_back(tid);
        const auto f = makeReadFrame(tid, static_cast<uint16_t>(i % 64), 1);
        burst.append(reinterpret_cast<const char*>(f.data()), static_cast<int>(f.size()));
    }
    REQUIRE(ch.write(burst) == burst.size());

    // 收集全部响应，校验响应 tid 与请求 tid 集合一致（无重复、无错配）
    ModbusStreamAccumulator acc;
    std::set<uint16_t> gotTids;
    int frames = collectFrames(ch, acc, 100, 5000,
        [&](uint16_t tid, const std::vector<uint8_t>& frame) { gotTids.insert(tid); });
    REQUIRE(frames == 100);                       // 全部响应到达

    const std::set<uint16_t> expected(tids.begin(), tids.end());
    CHECK(gotTids == expected);                   // 一一配对、无重复 ID、无多余帧

    // 逐个 release（正常事务闭环）
    for (const uint16_t tid : tids) alloc.release(tid);
    // 释放后可再次分配（复用路径）
    const uint16_t next = alloc.allocate();
    CHECK(next != 0);

    ch.close();
    server.close();
}