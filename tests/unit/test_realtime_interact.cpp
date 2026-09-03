// tests/unit/test_realtime_interact.cpp —— 实时曲线交互（切片 39，FR-RT-06/07/08）冒烟。
// 覆盖：① formatHoverLine 纯函数（时间/通道/值格式）
//       ② FR-RT-08 分轴：通道切右轴 → hasRightAxis；切回左轴 → 消失（不崩）
//       ③ FR-RT-07 标尺开关 + 读值文本（feed 样本后 notifyMouseMoved）
//       ④ ChartWidget 订阅流自动建通道行（bus.broadcast → 列表同步）
// ⚠ Q_OBJECT 布局改动须 clean 重编（项目铁律）。

#include <catch2/catch_test_macros.hpp>

#include <QApplication>

#include "charts/RealtimeChartWidget.h"
#include "charts/RealtimePlotWidget.h"
#include "datahub/DataBus.h"
#include "datahub/Sample.h"

using ens::datahub::DataBus;
using ens::datahub::Sample;
using ens::ui::RealtimeChartWidget;
using ens::ui::RealtimePlotWidget;

namespace {

Sample mkSample(uint32_t pid, uint64_t ts, double v) {
    Sample s{};
    s.pointId = pid;
    s.timestamp = ts;
    s.value = static_cast<float>(v);
    return s;
}
void pump(int ms = 100) { QApplication::processEvents(QEventLoop::AllEvents, ms); }

}  // namespace

TEST_CASE("realtime plot: format hover line pure function", "[ui][rt-interact][tier2]") {
    // 固定时刻 2026-09-02 10:00:00.000 UTC+8 无涉（只测格式含时间/名/值）
    const QString line = RealtimePlotWidget::formatHoverLine(
        1756800000000LL, QStringLiteral("MaxTemp"), 34.25);
    REQUIRE(line.contains(QStringLiteral("MaxTemp")));
    REQUIRE(line.contains(QStringLiteral("34.25")));
    REQUIRE(line.contains(QStringLiteral("=")));
}

TEST_CASE("realtime plot: channel axis switch to right creates right axis",
          "[ui][rt-interact][tier2]") {
    RealtimePlotWidget w;
    w.resize(800, 400);
    w.show();
    pump();

    w.addChannel(1, QStringLiteral("pt 1"), QColor());
    w.addChannel(2, QStringLiteral("pt 2"), QColor());
    REQUIRE(w.channelCount() == 2);
    REQUIRE_FALSE(w.hasRightAxis());

    w.setChannelAxis(1, RealtimePlotWidget::AxisSide::Right);
    REQUIRE(w.hasRightAxis());                    // FR-RT-08：右轴出现
    w.setChannelAxis(1, RealtimePlotWidget::AxisSide::Left);
    REQUIRE_FALSE(w.hasRightAxis());              // 无右轴通道 → 右轴不占用

    w.setChannelAxis(9999, RealtimePlotWidget::AxisSide::Right);   // 未知通道忽略
    pump();
}

TEST_CASE("realtime plot: ruler toggle renders multi-channel readout",
          "[ui][rt-interact][tier2]") {
    RealtimePlotWidget w;
    w.resize(800, 400);
    w.show();
    pump();

    w.addChannel(1, QStringLiteral("pt 1"), QColor());
    w.addChannel(2, QStringLiteral("pt 2"), QColor());
    // feed 样本后显式刷一帧（offscreen 下 30Hz timer 不触发；refreshNow 同函数体）
    const uint64_t base = 1756800000000ULL;
    for (int i = 0; i < 30; ++i) {
        w.onNewSample(1, 10.0 + i, static_cast<qint64>(base + i * 100));
        w.onNewSample(2, 20.0 + i, static_cast<qint64>(base + i * 100));
    }
    w.refreshNow();

    w.setRulerEnabled(true);
    REQUIRE(w.rulerEnabled());
    w.notifyMouseMoved(w.width() / 2);            // 中位悬停 → 标尺读值
    pump();
    // 标尺文本应含时间前缀与至少一条通道读数
    REQUIRE(w.hoverText().contains(QStringLiteral("t = ")));
    REQUIRE(w.hoverText().contains(QStringLiteral("pt 1")));

    w.setRulerEnabled(false);
    REQUIRE_FALSE(w.rulerEnabled());
    pump();
    w.close();
}

TEST_CASE("realtime chart: bus stream auto-creates channel list rows",
          "[ui][rt-interact][tier2]") {
    DataBus bus;
    RealtimeChartWidget w(&bus);
    w.resize(900, 400);
    w.show();
    pump();

    bus.broadcast(mkSample(11, 1756800000000ULL, 1.0));
    bus.broadcast(mkSample(22, 1756800000000ULL + 100, 2.0));
    pump();                                       // Queued → 建通道 + 列表行

    REQUIRE(w.channelListCount() == 2);
    w.close();
    pump();
}
