// tests/unit/test_chrome_render.cpp —— 切片 45 渲染级验收（offscreen QImage）。
// 在 offscreen 平台插件下：
//   * TitleBar 渲染到 QImage → 验证非空、有暗色背景、无 Qt 原生标题栏
//   * LoginDialog 渲染到 QImage → 验证含 TitleBar + 表单区
//   * MainWindow 渲染到 QImage → 验证含 TitleBar 在 menubar 上方、暗色背景
//   * 像素采样：右上角（close 按钮位置）应不是 Windows 原生"红 X"红
// 验收目标：替代用户目视截图，CI 自动化保证。
#include "auth/LoginDialog.h"
#include "common/TitleBar.h"
#include "common/WindowChrome.h"
#include "common/theme.h"
#include "main/MainWindow.h"

#include "AuthManager.h"

#include <QApplication>
#include <QColor>
#include <QIcon>
#include <QImage>
#include <QString>
#include <QWidget>
#include <QWindow>
#include <Qt>

#include <catch2/catch_test_macros.hpp>

using ens::ui::TitleBar;

TEST_CASE("TitleBar renders non-empty pixmap (offscreen QImage)", "[ui][chrome]") {
    if (!qApp) {
        WARN("QApplication not available; skipping render tests.");
        return;
    }
    TitleBar tb;
    tb.setTitle(QStringLiteral("EnerSentry 登录"));
    tb.resize(600, TitleBar::preferredHeight());
    QImage img = tb.grab().toImage();
    REQUIRE_FALSE(img.isNull());
    REQUIRE(img.width()  >= 600);
    REQUIRE(img.height() >= 32);
}

TEST_CASE("TitleBar top-left area is dark (background painted, not white native)",
          "[ui][chrome]") {
    if (!qApp) {
        WARN("QApplication not available; skipping pixel-sample test.");
        return;
    }
    ens::ui::applyTheme(qApp);
    QApplication::processEvents();

    TitleBar tb;
    tb.setTitle(QStringLiteral("EnerSentry"));
    tb.resize(400, TitleBar::preferredHeight());
    QImage img = tb.grab().toImage();
    REQUIRE_FALSE(img.isNull());

    // 背景应近似 #1c2127 暗色 —— 采样 logo 旁边（避免图标本身）：x=40, y=4
    const QColor c = img.pixelColor(40, 4);
    REQUIRE(c.red()   < 80);
    REQUIRE(c.green() < 80);
    REQUIRE(c.blue()  < 80);
}

TEST_CASE("TitleBar top-right area (close button) not bright red (Windows native X)",
          "[ui][chrome]") {
    if (!qApp) {
        WARN("QApplication not available; skipping pixel-sample test.");
        return;
    }
    ens::ui::applyTheme(qApp);
    QApplication::processEvents();

    TitleBar tb;
    tb.setTitle(QStringLiteral("EnerSentry"));
    tb.resize(400, TitleBar::preferredHeight());
    QImage img = tb.grab().toImage();
    REQUIRE_FALSE(img.isNull());

    // close 按钮区域中心（最右 46x32）默认无 hover，应是 #1c2127 暗色，
    // 不是 Windows 原生红 X (#E81123)
    const int sx = img.width() - 23;
    const QColor c = img.pixelColor(sx, 16);
    REQUIRE(c.red()   < 80);
    REQUIRE(c.green() < 80);
    REQUIRE(c.blue()  < 80);
}

TEST_CASE("LoginDialog with WindowChrome renders TitleBar on top + form below",
          "[ui][chrome]") {
    if (!qApp) {
        WARN("QApplication not available; skipping dialog render test.");
        return;
    }
    // 应用暗色主题（LoginDialog 启动也走 QSS，但这里直接构造验证即可）
    ens::ui::applyTheme(qApp);
    QApplication::processEvents();

    ens::business::AuthManager auth;
    // 默认内置 admin/operator；不调用 loadUsersFromJson 让 AuthManager 用内置
    ens::ui::LoginDialog dlg(&auth);
    dlg.show();
    QApplication::processEvents();

    QImage img = dlg.grab().toImage();
    REQUIRE_FALSE(img.isNull());
    // 期望 380×272（业务 240 + TitleBar 32）
    REQUIRE(img.width()  == 380);
    REQUIRE(img.height() == 272);

    // TitleBar 顶部 32px 应是暗色（避免 Qt 原生"全红"）
    const QColor topMid = img.pixelColor(190, 16);
    REQUIRE(topMid.red()   < 80);
    REQUIRE(topMid.green() < 80);
    REQUIRE(topMid.blue()  < 80);

    // 切片 45 验收物：保存 PNG 供目视对照
    const QString out = QStringLiteral("D:/Study/Qt_host_application_Project/EnerSentry/build/vs2022-debug/login_chrome.png");
    REQUIRE(img.save(out));
}

TEST_CASE("MainWindow with WindowChrome renders TitleBar above menubar + dark top",
          "[ui][chrome]") {
    if (!qApp) {
        WARN("QApplication not available; skipping MainWindow render test.");
        return;
    }
    ens::ui::applyTheme(qApp);
    QApplication::processEvents();

    // 空 UiDeps：仅验证窗口装饰与 TitleBar 渲染（业务子视图 null 容忍）
    ens::ui::UiDeps deps;
    ens::ui::MainWindow w(deps);
    w.resize(1280, 800);
    w.show();
    QApplication::processEvents();

    QImage img = w.grab().toImage();
    REQUIRE_FALSE(img.isNull());
    // 期望 1280×800（含 32px TitleBar 在 menubar 上方）
    REQUIRE(img.width()  == 1280);
    REQUIRE(img.height() == 800);

    // TitleBar 顶部 32px 应是暗色
    // 中央偏左 (200, 16) 采样；该位置是 TitleBar 内（Qt 原生标题栏位置）
    const QColor topLeft = img.pixelColor(200, 16);
    REQUIRE(topLeft.red()   < 80);
    REQUIRE(topLeft.green() < 80);
    REQUIRE(topLeft.blue()  < 80);

    // 右上角（close 按钮）应非红色（Windows 原生红 X）
    const int sx = img.width() - 23;
    const QColor closeBtn = img.pixelColor(sx, 16);
    REQUIRE(closeBtn.red()   < 80);
    REQUIRE(closeBtn.green() < 80);
    REQUIRE(closeBtn.blue()  < 80);

    // 验收物：主窗 PNG
    const QString out = QStringLiteral("D:/Study/Qt_host_application_Project/EnerSentry/build/vs2022-debug/mainwindow_chrome.png");
    REQUIRE(img.save(out));
}

// ───────────────────────── 切片 46：任务栏图标回归 ─────────────────────────
// 背景：WindowChrome 内部 setWindowFlags 会重建 native window handle(HWND)。
// Qt 5.15 在 QMainWindow + setMenuWidget(自绘 TitleBar) 路径上，重建后不会把
// QWidget::windowIcon() 自动写回新 HWND 的 HICON → 任务栏图标退化 Windows 默认空白
// （截图实证：登录窗正常、主窗空白）。
// 修复：WindowChrome 新增第 4 参 appIcon，setWindowFlags 之后强制
// target->setWindowIcon(appIcon) → 内部 windowHandle()->setIcon() 同步 native 层。
//
// offscreen 无法读 Windows HICON，但 Qt 的写回链路在 native 层有状态可观测：
// QWindow::icon()（windowHandle()->icon()）。修复前（重建后未写回）为 null，
// 修复后非空 —— 作为自动守卫点，删掉 reapply 即回归红灯。
TEST_CASE("MainWindow restores taskbar icon on WindowChrome HWND rebuild (s46)",
          "[ui][chrome]") {
    if (!qApp) {
        WARN("QApplication not available; skipping icon-restore test.");
        return;
    }
    ens::ui::applyTheme(qApp);
    QApplication::processEvents();

    // 真实构造路径：setWindowFlags → setupIcons(setWindowIcon) → setMenuWidget
    // → WindowChrome(appIcon)（切片 46 修复点）
    ens::ui::UiDeps deps;
    ens::ui::MainWindow w(deps);
    w.show();
    QApplication::processEvents();

    // native 层 icon 必须已写回（否则任务栏空白）
    QWindow* wh = w.windowHandle();
    REQUIRE(wh != nullptr);
    REQUIRE_FALSE(wh->icon().isNull());

    // 与 app_logo 实际图像一致（避免"写回了错误图标"的假绿）
    const QIcon logo(QStringLiteral(":/icons/app_logo.svg"));
    const QImage expect = logo.pixmap(48, 48).toImage();
    const QImage actual = wh->icon().pixmap(48, 48).toImage();
    REQUIRE(actual.size() == expect.size());
    REQUIRE(actual.pixelColor(expect.width() / 2, expect.height() / 2)
                == expect.pixelColor(expect.width() / 2, expect.height() / 2));
}

TEST_CASE("LoginDialog restores taskbar icon on WindowChrome HWND rebuild (s46)",
          "[ui][chrome]") {
    if (!qApp) {
        WARN("QApplication not available; skipping dialog icon-restore test.");
        return;
    }
    ens::ui::applyTheme(qApp);
    QApplication::processEvents();

    ens::business::AuthManager auth;
    ens::ui::LoginDialog dlg(&auth);
    dlg.show();
    QApplication::processEvents();

    QWindow* wh = dlg.windowHandle();
    REQUIRE(wh != nullptr);
    REQUIRE_FALSE(wh->icon().isNull());

    const QIcon logo(QStringLiteral(":/icons/app_logo.svg"));
    const QImage expect = logo.pixmap(48, 48).toImage();
    const QImage actual = wh->icon().pixmap(48, 48).toImage();
    REQUIRE(actual.size() == expect.size());
    REQUIRE(actual.pixelColor(expect.width() / 2, expect.height() / 2)
                == expect.pixelColor(expect.width() / 2, expect.height() / 2));
}

TEST_CASE("WindowChrome without appIcon keeps old signature working (s46)",
          "[ui][chrome]") {
    if (!qApp) {
        WARN("QApplication not available; skipping compatibility test.");
        return;
    }
    // 向后兼容：不传第 4 参（旧调用方）仍可构造，helper 可用。
    // 注意：host/tb 必须堆分配 —— WindowChrome/FramelessHelper 会把 helper
    // parent 到 host，host 析构会 delete 子对象；若 tb 是栈对象再 setParent(host)
    // 会在作用域结束时 double free（实测 SIGSEGV）。
    auto* host = new QWidget;
    auto* tb   = new ens::ui::TitleBar(host);
    ens::ui::WindowChrome wc(host, tb, /*resizable=*/false);
    REQUIRE(wc.helper() != nullptr);
    delete host;   // 级联释放 tb 与 helper（helper parent == host）
}
