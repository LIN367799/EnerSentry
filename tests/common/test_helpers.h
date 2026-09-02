// tests/common/test_helpers.h —— 集成测试公共工具（Tier 3 事件循环等待 / TCP 帧构造）。
// 各 integration 测试共用：QApplication 单例（切片 33：由 QCoreApplication 升级，
//   GUI 冒烟需 QWidget；offscreen 平台无窗口可跑）、信号等待器、FC03 读请求帧组帧。
#pragma once

#include <QApplication>
#include <QEventLoop>
#include <QTimer>

#include <array>
#include <cstdint>
#include <vector>

#include "core/mbap.h"

namespace ens::test {

// QApplication 单例（QTimer/事件循环/QWidget 前提；QCoreApplication 兼容语义）
inline QCoreApplication* appInstance() {
    static QCoreApplication* app = [] {
        static int argc = 1;
        static char arg0[] = "ens_tests";
        static char* argv[] = {arg0, nullptr};
        // 切片 33：GUI 测试无窗口环境跑 → offscreen 平台（QApplication 构造前必须设置）
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
        return new QApplication(argc, argv);
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