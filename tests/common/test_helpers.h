// tests/common/test_helpers.h —— 集成测试公共工具（Tier 3 事件循环等待 / TCP 帧构造）。
// 各 integration 测试共用：QCoreApplication 单例（Catch2 进程内只能建一次）、
// 信号等待器（新式 connect + 具体 sender 类型）、FC03 读请求帧组帧。
#pragma once

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <array>
#include <cstdint>
#include <vector>

#include "core/mbap.h"

namespace ens::test {

// QCoreApplication 单例（QTimer/事件循环前提）
inline QCoreApplication* appInstance() {
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

// FC03 读请求帧（MBAP + PDU），完整 wire 帧
inline std::vector<uint8_t> makeReadFrame(uint16_t tid, uint16_t addr, uint16_t qty) {
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

}  // namespace ens::test