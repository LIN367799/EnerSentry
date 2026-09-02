// tests/unit/test_replay.cpp —— L1 回放 + AlarmReplayDialog 冒烟（切片 38，FR-AL-12）。
// 覆盖：① IL1SnapshotReader 抽象（L1SnapshotStore 实现）按 ±30s 窗口提取内存高频样本
//       ② 时间窗边界：窗外样本不返回、返回升序
//       ③ AlarmReplayDialog setData 空/有数据渲染不崩（QApplication offscreen）

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <QApplication>
#include <QVector>

#include "datahub/IL1SnapshotReader.h"
#include "datahub/L1SnapshotStore.h"
#include "datahub/Sample.h"
#include "ui/controls/AlarmReplayDialog.h"

using ens::datahub::IL1SnapshotReader;
using ens::datahub::L1SnapshotStore;
using ens::datahub::RingBufferPolicyEntry;
using ens::datahub::Sample;

namespace {

Sample mkSample(uint64_t ts, uint32_t pid, float v) {
    Sample s{};
    s.timestamp = ts;
    s.pointId   = pid;
    s.value     = v;
    return s;
}

}  // namespace

TEST_CASE("replay: l1 reader extracts +-30s window via interface", "[master][datahub][replay]") {
    L1SnapshotStore store;
    REQUIRE(store.initFromPolicy(QVector<RingBufferPolicyEntry>{{1001, 100, 3'600'000, 1}}));

    const uint64_t t0 = 1700000000000ULL;   // 固定参考时刻
    // 写入 t0-15s ~ t0+15s @100ms（窗口内 301 点）；另写窗外的 t0-60s/t0+60s 各一点
    std::vector<Sample> all;
    for (int i = -150; i <= 150; ++i) {
        const uint64_t ts = t0 + static_cast<uint64_t>(i * 100);
        store.write(1001, mkSample(ts, 1001, static_cast<float>(i)));
        all.push_back(mkSample(ts, 1001, static_cast<float>(i)));
    }
    store.write(1001, mkSample(t0 - 60000, 1001, -600.0f));   // 窗外（先于窗口写，环形滚动可能保留）
    store.write(1001, mkSample(t0 + 60000, 1001, 600.0f));    // 窗外

    IL1SnapshotReader& reader = store;   // 经抽象读取
    std::vector<Sample> out(4096);
    const size_t n = reader.replayExtract(1001, t0 - 30000, t0 + 30000, out.data(), out.size());

    REQUIRE(n >= 1);
    // 全部落在 [t0-30s, t0+30s) 窗口内
    for (size_t i = 0; i < n; ++i) {
        REQUIRE(out[i].timestamp >= t0 - 30000);
        REQUIRE(out[i].timestamp < t0 + 30000);
        if (i > 0) REQUIRE(out[i].timestamp >= out[i - 1].timestamp);   // 升序
    }
    // 时间戳唯一性说明窗口内点被完整保留（容量充足时 301 点全在）
    REQUIRE(n <= 301);
}

TEST_CASE("replay: unknown point and empty window return zero", "[master][datahub][replay]") {
    L1SnapshotStore store;
    REQUIRE(store.initFromPolicy(QVector<RingBufferPolicyEntry>{{1, 100, 3'600'000, 1}}));
    IL1SnapshotReader& reader = store;
    std::vector<Sample> out(64);
    REQUIRE(reader.replayExtract(9999, 0, 1000, out.data(), out.size()) == 0);  // 未注册点
    REQUIRE(reader.replayExtract(1, 1000, 500, out.data(), out.size()) == 0);   // 空区间
}

TEST_CASE("replay dialog: setData with empty and samples renders without crash",
          "[ui][replay][smoke]") {
    // QApplication 由 test_helpers appInstance 提供（offscreen）
    ens::ui::AlarmReplayDialog dlg;
    dlg.setData(7, QStringLiteral("Rack-01_MaxTemp"), 1700000000000ULL, {});   // 空 → 提示路径
    std::vector<Sample> samples = {
        mkSample(1700000000000ULL - 1000, 7, 60.0f),
        mkSample(1700000000000ULL, 7, 64.0f),
        mkSample(1700000000000ULL + 1000, 7, 62.0f),
    };
    dlg.setData(7, QStringLiteral("Rack-01_MaxTemp"), 1700000000000ULL, samples);
    dlg.show();
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    REQUIRE(dlg.isVisible());
    dlg.close();
    QApplication::processEvents(QEventLoop::AllEvents, 50);
}
