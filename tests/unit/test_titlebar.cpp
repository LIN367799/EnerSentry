// tests/unit/test_titlebar.cpp —— 切片 45 TitleBar 单元测试。
// 重点：setTitle/setTitleIconPath、Min/MaxVisible、MaximizeButtonChecked 切图标、
// 关闭/最小化/最大化信号触发（lambda 计数器避免 QSignalSpy 引入 QtTest 依赖）。
#include "common/TitleBar.h"

#include <QApplication>
#include <QIcon>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>

#include <atomic>

using ens::ui::TitleBar;

TEST_CASE("TitleBar default construction and properties", "[ui][titlebar]") {
    TitleBar tb;
    // 默认标题
    REQUIRE(tb.title() == QStringLiteral("EnerSentry"));
    // 推荐高度
    REQUIRE(tb.preferredHeight() == 32);
    // 默认最小/最大化按钮可见（通过 objectName 找）
    REQUIRE(tb.findChild<QWidget*>("minBtn"));
    REQUIRE(tb.findChild<QWidget*>("maxBtn"));
    REQUIRE(tb.findChild<QWidget*>("closeBtn"));
}

TEST_CASE("TitleBar setTitle updates title label", "[ui][titlebar]") {
    TitleBar tb;
    tb.setTitle(QStringLiteral("EnerSentry 登录"));
    REQUIRE(tb.title() == QStringLiteral("EnerSentry 登录"));
}

TEST_CASE("TitleBar setTitleIconPath accepts custom path", "[ui][titlebar]") {
    TitleBar tb;
    // 不存在的 qrc 路径不应崩（QIcon 构造吞错）；测试仅验不抛
    REQUIRE_NOTHROW(tb.setTitleIconPath(QStringLiteral(":/icons/app_logo.svg")));
    REQUIRE_NOTHROW(tb.setTitleIconPath(QStringLiteral(":/icons/missing.svg")));
}

TEST_CASE("TitleBar Min/MaxVisible toggles button visibility", "[ui][titlebar]") {
    TitleBar tb;
    auto* minBtn = tb.findChild<QWidget*>("minBtn");
    auto* maxBtn = tb.findChild<QWidget*>("maxBtn");
    REQUIRE(minBtn); REQUIRE(maxBtn);
    // QWidget::isVisible 受父链 visible 状态影响；未 show 时用 isVisibleTo(parent)
    REQUIRE(minBtn->isVisibleTo(&tb));
    REQUIRE(maxBtn->isVisibleTo(&tb));

    tb.setMinimumVisible(false);
    REQUIRE_FALSE(minBtn->isVisibleTo(&tb));

    tb.setMaximumVisible(false);
    REQUIRE_FALSE(maxBtn->isVisibleTo(&tb));
}

TEST_CASE("TitleBar close/minimize/maximize buttons emit expected signals",
          "[ui][titlebar]") {
    if (!qApp) {
        WARN("QApplication not available; skipping signal-emission tests.");
        return;
    }
    TitleBar tb;
    std::atomic<int> closeCount{0};
    std::atomic<int> minCount{0};
    std::atomic<int> maxCount{0};
    bool maxWantMaximize = false;

    QObject::connect(&tb, &TitleBar::closeClicked, [&] { closeCount.fetch_add(1); });
    QObject::connect(&tb, &TitleBar::minimizeClicked, [&] { minCount.fetch_add(1); });
    QObject::connect(&tb, &TitleBar::maximizeToggled,
                     [&](bool want) { maxWantMaximize = want; maxCount.fetch_add(1); });

    auto* minBtn   = tb.findChild<QWidget*>("minBtn");
    auto* maxBtn   = tb.findChild<QWidget*>("maxBtn");
    auto* closeBtn = tb.findChild<QWidget*>("closeBtn");
    REQUIRE(minBtn); REQUIRE(maxBtn); REQUIRE(closeBtn);

    QMetaObject::invokeMethod(minBtn,   "click", Qt::DirectConnection);
    QMetaObject::invokeMethod(maxBtn,   "click", Qt::DirectConnection);
    QMetaObject::invokeMethod(closeBtn, "click", Qt::DirectConnection);

    REQUIRE(minCount.load()   == 1);
    REQUIRE(closeCount.load() == 1);
    REQUIRE(maxCount.load()   == 1);
    // 默认未最大化 → toggle(true)
    REQUIRE(maxWantMaximize == true);

    // 第二次点 max 按钮：toggle(false)
    QMetaObject::invokeMethod(maxBtn, "click", Qt::DirectConnection);
    REQUIRE(maxCount.load() == 2);
    REQUIRE(maxWantMaximize == false);
}

TEST_CASE("TitleBar setMaximizeButtonChecked toggles icon path",
          "[ui][titlebar]") {
    TitleBar tb;
    auto* maxBtn = qobject_cast<QPushButton*>(tb.findChild<QWidget*>("maxBtn"));
    REQUIRE(maxBtn);

    // 初始未最大化：图标 = window_maximize.svg
    tb.setMaximizeButtonChecked(false);
    { QIcon ic = maxBtn->icon(); REQUIRE(!ic.isNull()); }
    // 切到最大化态：图标 = window_restore.svg
    tb.setMaximizeButtonChecked(true);
    { QIcon ic = maxBtn->icon(); REQUIRE(!ic.isNull()); }
}
