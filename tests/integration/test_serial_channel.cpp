// tests/integration/test_serial_channel.cpp
// Phase 1 2.1.2 + B4 Tier 3：SerialChannel（主站）⇄ RtuSlavePort（从站）真实虚拟串口回环。
// com0com 端口对：环境变量 ENS_SIM_COM_A/B 优先，未设置时探测候选对（COM5/COM6 等）；
// 端口不可用则 SKIP（不误报失败——Track B 踩坑：com0com 未就绪时降级）。
// 验证：CRC 组帧/响应字节/收发对称 / 坏 CRC 拒绝 / close 幂等。

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QEventLoop>
#include <QSerialPortInfo>
#include <QTimer>

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "core/crc16.h"
#include "sim/rtu_slave_port.h"
#include "SerialChannel.h"

using ens::channel::ChannelConfig;
using ens::channel::ChannelType;
using ens::channel::SerialChannel;
using ens::channel::SerialConfig;
using ens::sim::RtuSlavePort;

namespace {

QCoreApplication* appInstance() {
    static QCoreApplication* app = [] {
        static int argc = 1;
        static char arg0[] = "ens_tests";
        static char* argv[] = {arg0, nullptr};
        return new QCoreApplication(argc, argv);
    }();
    return app;
}

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

bool portExists(const QString& name) {
    for (const auto& info : QSerialPortInfo::availablePorts())
        if (info.portName() == name) return true;
    return false;
}

// 端口对解析：环境变量 > 候选对（COM5/COM6 等 com0com 常见分配）
std::pair<QString, QString> resolvePair() {
    const QString a = qEnvironmentVariable("ENS_SIM_COM_A");
    const QString b = qEnvironmentVariable("ENS_SIM_COM_B");
    if (!a.isEmpty() && !b.isEmpty() && a != b && portExists(a) && portExists(b))
        return {a, b};
    const std::vector<std::pair<QString, QString>> cands = {
        {"COM5", "COM6"}, {"COM3", "COM4"}, {"COM4", "COM3"}, {"COM6", "COM5"}};
    for (const auto& [x, y] : cands)
        if (portExists(x) && portExists(y)) return {x, y};
    return {};
}

ChannelConfig serialConfig(const QString& portName) {
    ChannelConfig cfg;
    cfg.type = ChannelType::Serial;
    SerialConfig s;
    s.portName = portName;
    s.baudRate = 115200;
    cfg.payload = s;
    return cfg;
}

// RTU 读请求帧：addr + fc + addrHi/Lo + qtyHi/Lo + crc（低字节在前）
std::vector<uint8_t> makeRtuReadReq(uint8_t addr, uint16_t regAddr, uint16_t qty) {
    std::vector<uint8_t> f = {addr, 0x03,
                              static_cast<uint8_t>(regAddr >> 8), static_cast<uint8_t>(regAddr & 0xFF),
                              static_cast<uint8_t>(qty >> 8), static_cast<uint8_t>(qty & 0xFF)};
    const uint16_t crc = ens::core::crc16_modbus(f.data(), f.size());
    f.push_back(static_cast<uint8_t>(crc & 0xFF));
    f.push_back(static_cast<uint8_t>(crc >> 8));
    return f;
}

}  // namespace

TEST_CASE("serial_channel: RTU round-trip over virtual com pair", "[serial][rtu]") {
    appInstance();
    const auto [slavePort, masterPort] = resolvePair();
    if (slavePort.isEmpty()) SKIP("no com0com pair detected; set ENS_SIM_COM_A/B (e.g. COM5,COM6)");

    RtuSlavePort slave(slavePort.toStdString(), 115200);
    REQUIRE(slave.open());
    slave.regs().set(0, 0x1357);
    slave.regs().set(1, 0xABCD);

    SerialChannel ch;
    SignalWaiter conn(&ch, &SerialChannel::connectionChanged);
    REQUIRE(ch.open(serialConfig(masterPort)));
    REQUIRE(conn.wait(2000));                          // 串口 open 即连接
    REQUIRE(ch.isConnected());

    // 读 addr=0 qty=2 → 期望 PDU [0x03, 0x04, 0x13, 0x57, 0xAB, 0xCD]
    const auto req = makeRtuReadReq(1, 0, 2);
    const int written = ch.write(QByteArray(reinterpret_cast<const char*>(req.data()),
                                            static_cast<int>(req.size())));
    CHECK(written == static_cast<int>(req.size()));

    SignalWaiter rx(&ch, &SerialChannel::dataReceived);
    REQUIRE(rx.wait(2000));                            // 从站响应到达
    const QByteArray resp = ch.read(4096);
    REQUIRE(resp.size() == 9);                         // addr(1)+fc(1)+byteCount(1)+data(4)+crc(2)
    CHECK(static_cast<uint8_t>(resp.at(0)) == 1);      // 从站地址
    CHECK(static_cast<uint8_t>(resp.at(1)) == 0x03);
    CHECK(static_cast<uint8_t>(resp.at(2)) == 4);      // byteCount
    CHECK(static_cast<uint8_t>(resp.at(3)) == 0x13);   // 0x1357 大端
    CHECK(static_cast<uint8_t>(resp.at(4)) == 0x57);
    CHECK(static_cast<uint8_t>(resp.at(5)) == 0xAB);   // 0xABCD
    CHECK(static_cast<uint8_t>(resp.at(6)) == 0xCD);

    // 响应 CRC 校验通过（方言一致，§2C CRC 方言行）
    const uint8_t* p = reinterpret_cast<const uint8_t*>(resp.constData());
    const uint16_t calc = ens::core::crc16_modbus(p, 7);
    const uint16_t recv = static_cast<uint16_t>(p[7] | (p[8] << 8));
    CHECK(calc == recv);

    // 收发对称
    const auto& s = ch.getStats();
    CHECK(s.bytesSent.load() == static_cast<uint64_t>(req.size()));
    CHECK(s.bytesReceived.load() == static_cast<uint64_t>(resp.size()));

    ch.close();
    slave.close();
}

TEST_CASE("serial_channel: corrupted CRC request rejected by slave", "[serial][rtu]") {
    appInstance();
    const auto [slavePort, masterPort] = resolvePair();
    if (slavePort.isEmpty()) SKIP("no com0com pair detected; set ENS_SIM_COM_A/B (e.g. COM5,COM6)");

    RtuSlavePort slave(slavePort.toStdString(), 115200);
    REQUIRE(slave.open());

    SerialChannel ch;
    SignalWaiter conn(&ch, &SerialChannel::connectionChanged);
    REQUIRE(ch.open(serialConfig(masterPort)));
    REQUIRE(conn.wait(2000));

    auto bad = makeRtuReadReq(1, 0, 1);
    bad.back() ^= 0xFF;                                // 翻转 CRC 高字节 → 假坏帧
    REQUIRE(ch.write(QByteArray(reinterpret_cast<const char*>(bad.data()),
                                static_cast<int>(bad.size()))) == static_cast<int>(bad.size()));

    // 从站 CRC 校验失败 → 不应有响应：500ms 内无 dataReceived 即通过
    SignalWaiter rx(&ch, &SerialChannel::dataReceived);
    CHECK_FALSE(rx.wait(500));
    CHECK(ch.read(4096).isEmpty());

    ch.close();
    slave.close();
}

TEST_CASE("serial_channel: close is idempotent", "[serial][rtu]") {
    appInstance();
    const auto [slavePort, masterPort] = resolvePair();
    if (slavePort.isEmpty()) SKIP("no com0com pair detected; set ENS_SIM_COM_A/B (e.g. COM5,COM6)");

    RtuSlavePort slave(slavePort.toStdString(), 115200);
    REQUIRE(slave.open());

    SerialChannel ch;
    SignalWaiter conn(&ch, &SerialChannel::connectionChanged);
    REQUIRE(ch.open(serialConfig(masterPort)));
    REQUIRE(conn.wait(2000));

    ch.close();
    ch.close();                                        // 幂等
    CHECK_FALSE(ch.isConnected());

    slave.close();
}