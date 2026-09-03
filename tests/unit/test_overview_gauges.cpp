// tests/unit/test_overview_gauges.cpp —— 总览仪表/热力条（切片 40，FR-OV-06/07）。
// 覆盖：① ovrClassifyName 点分类纯函数（SOC/簇 MaxTemp/非簇）
//       ② SocGauge setValue 钳制 + 渲染不崩
//       ③ TempHeatBar 色阶语义（高温红系 > 低温） + hover 读值
//       ④ OverviewWidget bus 集成：SOC 均值（88+62)/2=75 + 簇温度格 2

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

#include <QApplication>
#include <QFile>
#include <QPixmap>
#include <QTemporaryDir>

#include "ui/common/OverviewPoints.h"
#include "ui/controls/SocGauge.h"
#include "ui/controls/TempHeatBar.h"
#include "ui/views/overview_widget.h"

#include "datahub/DataBus.h"
#include "datahub/Sample.h"
#include "protocol/PointTable.h"

using ens::datahub::DataBus;
using ens::datahub::Sample;
using ens::protocol::PointTable;
using ens::ui::OvrKind;
using ens::ui::OverviewWidget;
using ens::ui::ovrClassifyName;
using ens::ui::SocGauge;
using ens::ui::TempHeatBar;

namespace {

void pump(int ms = 700) { QApplication::processEvents(QEventLoop::AllEvents, ms); }

const char* kFourPointJson = R"json({
 "meta": {"schemaVersion": "1.1", "generator": "test_overview_gauges", "deviceCount": 2, "pointCount": 4},
 "points": [
  {"pointId": 1, "pointName": "Rack-01_MaxTemp", "linkId": 1, "slaveAddress": 1, "regType": "HoldingRegister", "registerAddr": 4096, "dataType": "Float32", "byteOrder": "ABCD", "scaleFactor": 0.1, "offset": 0.0, "unit": "C", "pollIntervalMs": 1000, "priority": 1, "enabled": true},
  {"pointId": 2, "pointName": "Rack-01_SOC", "linkId": 1, "slaveAddress": 1, "regType": "HoldingRegister", "registerAddr": 4098, "dataType": "Float32", "byteOrder": "ABCD", "scaleFactor": 0.01, "offset": 0.0, "unit": "%", "pollIntervalMs": 1000, "priority": 1, "enabled": true},
  {"pointId": 26, "pointName": "Rack-02_MaxTemp", "linkId": 2, "slaveAddress": 2, "regType": "HoldingRegister", "registerAddr": 5632, "dataType": "Float32", "byteOrder": "ABCD", "scaleFactor": 0.1, "offset": 0.0, "unit": "C", "pollIntervalMs": 1000, "priority": 1, "enabled": true},
  {"pointId": 27, "pointName": "Rack-02_SOC", "linkId": 2, "slaveAddress": 2, "regType": "HoldingRegister", "registerAddr": 5634, "dataType": "Float32", "byteOrder": "ABCD", "scaleFactor": 0.01, "offset": 0.0, "unit": "%", "pollIntervalMs": 1000, "priority": 1, "enabled": true}
 ]
})json";

std::shared_ptr<PointTable> makePointTable(const QString& dir) {
    const QString path = dir + QStringLiteral("/pt.json");
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(kFourPointJson);
    f.close();
    auto pt = PointTable::loadFromJsonFile(std::filesystem::path(path.toStdWString()));
    REQUIRE(pt != nullptr);
    return pt;
}

Sample mkSample(uint32_t pid, float v) {
    Sample s{};
    s.pointId = pid;
    s.timestamp = 1756800000000ULL;
    s.value = v;
    return s;
}

}  // namespace

TEST_CASE("overview points: classify rack soc and max temp by name",
          "[ui][overview][tier2]") {
    const auto soc1 = ovrClassifyName(QStringLiteral("Rack-01_SOC"));
    REQUIRE(soc1.kind == OvrKind::Soc);
    REQUIRE(soc1.rackNo == 1);

    const auto t2 = ovrClassifyName(QStringLiteral("Rack-02_MaxTemp"));
    REQUIRE(t2.kind == OvrKind::ClusterMaxTemp);
    REQUIRE(t2.rackNo == 2);

    REQUIRE(ovrClassifyName(QStringLiteral("Rack-01_AvgTemp")).kind == OvrKind::None);
    REQUIRE(ovrClassifyName(QStringLiteral("Liquid_SupplyTemp")).kind == OvrKind::None);
    REQUIRE(ovrClassifyName(QStringLiteral("Rack_SOC")).kind == OvrKind::None);   // 无簇号
    REQUIRE(ovrClassifyName(QStringLiteral("Rack-01_TotalV")).kind == OvrKind::None);
}

TEST_CASE("soc gauge: clamps value and renders", "[ui][overview][tier2]") {
    SocGauge g;
    g.resize(240, 180);
    g.setValue(55.0);
    REQUIRE(g.value() == Catch::Approx(55.0));
    g.setValue(-5.0);
    REQUIRE(g.value() == Catch::Approx(0.0));    // 下钳
    g.setValue(123.0);
    REQUIRE(g.value() == Catch::Approx(100.0));  // 上钳
    QPixmap pm(240, 180);
    g.render(&pm);                               // paint 冒烟不崩
}

TEST_CASE("temp heat bar: color gradient and hover readout", "[ui][overview][tier2]") {
    // 高温格 red 分量显著高于低温格；低温蓝分量显著高
    const QColor hot = TempHeatBar::tempColor(62.0f);
    const QColor warm = TempHeatBar::tempColor(48.0f);
    const QColor cool = TempHeatBar::tempColor(33.0f);
    REQUIRE(hot.red() > cool.red());
    REQUIRE(cool.blue() > hot.blue());
    REQUIRE(warm.red() > cool.red());

    TempHeatBar bar;
    bar.resize(300, 56);
    QVector<TempHeatBar::Cell> cells;
    cells.push_back({QStringLiteral("Rack-01"), 34.2f, true});
    cells.push_back({QStringLiteral("Rack-02"), 41.5f, true});
    cells.push_back({QStringLiteral("Rack-03"), 62.0f, true});
    bar.setCells(cells);
    bar.notifyMouseMoved(bar.width() / 6);       // 首格
    REQUIRE(bar.hoverText().contains(QStringLiteral("Rack-01")));
    REQUIRE(bar.hoverText().contains(QStringLiteral("34.2")));
    bar.notifyMouseMoved(bar.width() * 5 / 6);   // 末格
    REQUIRE(bar.hoverText().contains(QStringLiteral("Rack-03")));
}

TEST_CASE("overview widget: bus samples drive soc average and heat cells",
          "[ui][overview][tier2]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    auto pt = makePointTable(tmp.path());
    REQUIRE(pt->allPoints().size() == 4);

    DataBus bus;
    OverviewWidget w(&bus, pt);
    w.resize(900, 420);
    w.show();
    pump(100);

    bus.broadcast(mkSample(2, 88.0f));    // Rack-01_SOC
    bus.broadcast(mkSample(27, 62.0f));   // Rack-02_SOC
    bus.broadcast(mkSample(1, 34.2f));    // Rack-01_MaxTemp
    bus.broadcast(mkSample(26, 41.5f));   // Rack-02_MaxTemp
    // offscreen 下 500ms timer 触发不确定（同切片 39 教训）→ 显式 refreshNow 同步收口
    w.refreshNow();
    REQUIRE(w.lastSoc() == Catch::Approx(75.0));   // (88+62)/2
    REQUIRE(w.heatCellCount() == 2);
    w.close();
    pump(50);
}
