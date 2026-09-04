// tests/unit/test_frameless_helper.cpp —— 切片 45 FramelessHelper 单元测试。
// 重点：hitTestZone 9 区判定、zoneToEdges、setResizable/Movable/Snap 状态。
// eventFilter 的拖拽/缩放不在此测（offscreen 平台下 mouse 事件依赖 GUI 平台，
// 真实拖拽测在集成测试 / GUI 冒烟）。
#include "common/FramelessHelper.h"

#include <QPoint>
#include <QSize>
#include <QWidget>
#include <Qt>

#include <catch2/catch_test_macros.hpp>

using ens::ui::FramelessHelper;

TEST_CASE("FramelessHelper hitTestZone 9 zones (offscreen target 400x300)",
          "[ui][frameless]") {
    QWidget target;
    target.resize(400, 300);
    FramelessHelper h(&target, /*dragRegion=*/nullptr, &target);
    h.setBorderWidth(6);

    // 四角
    REQUIRE(h.hitTestZone(QPoint(0, 0))    == 4);   // NW
    REQUIRE(h.hitTestZone(QPoint(399, 0))  == 5);   // NE
    REQUIRE(h.hitTestZone(QPoint(0, 299))  == 6);   // SW
    REQUIRE(h.hitTestZone(QPoint(399, 299))== 7);   // SE
    // 四边中点
    REQUIRE(h.hitTestZone(QPoint(200, 0))  == 0);   // N
    REQUIRE(h.hitTestZone(QPoint(200, 299))== 1);   // S
    REQUIRE(h.hitTestZone(QPoint(0, 150))  == 2);   // W
    REQUIRE(h.hitTestZone(QPoint(399, 150))== 3);   // E
    // 中心
    REQUIRE(h.hitTestZone(QPoint(200, 150))== 8);   // none
}

TEST_CASE("FramelessHelper hitTestZone respects resizable=false", "[ui][frameless]") {
    QWidget target;
    target.resize(200, 200);
    FramelessHelper h(&target, nullptr, &target);
    h.setBorderWidth(6);
    h.setResizable(false);
    REQUIRE(h.hitTestZone(QPoint(0, 0)) == 8);
    REQUIRE(h.hitTestZone(QPoint(199, 199)) == 8);
}

TEST_CASE("FramelessHelper hitTestZone clamps border to half-size on tiny target",
          "[ui][frameless]") {
    QWidget target;
    target.resize(20, 20);   // border=6，钳到 10（=20/2），中心 (10, 10) 在 4 边中间 → none
    FramelessHelper h(&target, nullptr, &target);
    h.setBorderWidth(6);
    // 角命中
    REQUIRE(h.hitTestZone(QPoint(0, 0))   == 4);
    REQUIRE(h.hitTestZone(QPoint(19, 19)) == 7);
    // 中心应仍为 none
    REQUIRE(h.hitTestZone(QPoint(10, 10)) == 8);
}

TEST_CASE("FramelessHelper zoneToEdges mapping", "[ui][frameless]") {
    using E = Qt::Edges;
    REQUIRE(FramelessHelper::zoneToEdges(0) == E(Qt::TopEdge));
    REQUIRE(FramelessHelper::zoneToEdges(1) == E(Qt::BottomEdge));
    REQUIRE(FramelessHelper::zoneToEdges(2) == E(Qt::LeftEdge));
    REQUIRE(FramelessHelper::zoneToEdges(3) == E(Qt::RightEdge));
    REQUIRE(FramelessHelper::zoneToEdges(4) == E(Qt::TopEdge | Qt::LeftEdge));
    REQUIRE(FramelessHelper::zoneToEdges(5) == E(Qt::TopEdge | Qt::RightEdge));
    REQUIRE(FramelessHelper::zoneToEdges(6) == E(Qt::BottomEdge | Qt::LeftEdge));
    REQUIRE(FramelessHelper::zoneToEdges(7) == E(Qt::BottomEdge | Qt::RightEdge));
    REQUIRE(FramelessHelper::zoneToEdges(8) == E(Qt::Edges{}));
    REQUIRE(FramelessHelper::zoneToEdges(-1) == E(Qt::Edges{}));
}

TEST_CASE("FramelessHelper setBorderWidth clamps to [1, 32]", "[ui][frameless]") {
    QWidget target;
    FramelessHelper h(&target, nullptr, &target);
    h.setBorderWidth(0);  REQUIRE(h.borderWidth() == 1);
    h.setBorderWidth(-5); REQUIRE(h.borderWidth() == 1);
    h.setBorderWidth(100);REQUIRE(h.borderWidth() == 32);
    h.setBorderWidth(8);  REQUIRE(h.borderWidth() == 8);
}

TEST_CASE("FramelessHelper resizable/movable/snap state getters", "[ui][frameless]") {
    QWidget target;
    FramelessHelper h(&target, nullptr, &target);
    REQUIRE(h.resizable());
    REQUIRE(h.movable());
    REQUIRE(h.snapToMaximize());
    h.setResizable(false); REQUIRE_FALSE(h.resizable());
    h.setMovable(false);   REQUIRE_FALSE(h.movable());
    h.setSnapToMaximize(false); REQUIRE_FALSE(h.snapToMaximize());
}

TEST_CASE("FramelessHelper null target is no-op (no crash)", "[ui][frameless]") {
    FramelessHelper h(nullptr, nullptr);
    REQUIRE(h.borderWidth() == 6);
    h.setResizable(false);
    h.setMovable(false);
    h.setSnapToMaximize(false);
    REQUIRE(h.hitTestZone(QPoint(0, 0)) == 8);
}
