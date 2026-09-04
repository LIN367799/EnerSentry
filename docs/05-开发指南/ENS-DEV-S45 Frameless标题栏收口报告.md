# 切片 45 收口报告 — 自绘 Frameless 标题栏（去除 Qt 原生"全红"窗口装饰）

> 版本：v1.0（2026-09-04）｜切片 45.1 → 45.3（3 切片）｜ctest 343/343 全绿｜构建 0 警告

## 1. 范围与目标

用户截图（@image#1）圈出登录窗顶部 30px 红框，**实质是 Qt 平台插件在 Windows 上给 `QDialog`/`QMainWindow` 渲染的原生窗口装饰条**（系统菜单 / 最大化 / 关闭按钮那一行）。问题本质是 Qt 5.15 平台插件行为不可控（受 Windows 当前主题影响）—— 即使替换为 ENS 暗色 QSS，**那块区域仍由 Windows 系统渲染**，与设计深蓝灰主题完全脱节，在高对比度主题下视觉上呈现"全红"。

| 切片 | 目标 | 状态 |
|---|---|---|
| 45.1 基础件 | 抽出 `FramelessHelper` + `TitleBar` + `WindowChrome` 三个 L5 UI 通用组件 + 4 枚 SVG 资源 + 17 个单测 | ✅ |
| 45.2 登录窗 | `LoginDialog` 接入 Frameless，自绘暗色 32px 标题栏（仅保留关闭按钮） | ✅ |
| 45.3 主窗 | `MainWindow` 接入 Frameless，`setMenuWidget(titleBar)` 把 TitleBar 置 menubar 上方；min/max/close 三件套 | ✅ |

**里程碑含义**：登录窗与主窗的"顶部全红"原生装饰被彻底替换为自绘暗色 TitleBar，与 `theme.qss` 调色板（`#1c2127` 暗蓝灰 + `#2a3038` 边框）无缝衔接；且三个通用组件完全可复用，后续 SessionLockDialog / CriticalAlarmDialog / About 对话框可一行接入。

## 2. 交付清单

| 切片 | 主题 | 关键交付 |
|---|---|---|
| 45.1 | 通用件 + 单测 | `common/FramelessHelper.{h,cpp}`（≈170 行；eventFilter 接管 9 区 hitTest + 边缘缩放 + 拖拽 + Aero Snap）；`common/TitleBar.{h,cpp}`（≈190 行；QWidget 自绘 32px，logo+title+min/max/close 五件套 + QSS 钩子 `objectName=#EnerSentryTitleBar`）；`common/WindowChrome.{h,cpp}`（≈40 行；薄封装）；4 枚 SVG（`window_minimize/maximize/restore/close.svg`，Feather 风格，qrc prefix `/icons` + alias 压平）；13 个单测（7 hitTestZone + 6 TitleBar 行为） |
| 45.2 | 登录窗 | `LoginDialog.{h,cpp,ui}` 改：构造前 `setWindowFlags(FramelessWindowHint)` → setupUi → 构造 TitleBar + `setMinimumVisible(false)` + `setMaximumVisible(false)`（Dialog 极简） → `insertWidget(0, tb)` → `WindowChrome(this, tb, resizable=false)`；`fixed 380×272`；`closeClicked → reject`；唯一 UI 改动：根 layout 顶部 margin=0 消除与 TitleBar 间隙 |
| 45.3 | 主窗 | `MainWindow.{h,cpp}` 改：构造前 `setWindowFlags(FramelessWindowHint)` → setupUi → `new TitleBar` + `setMenuWidget(tb)`（TitleBar 置 menubar 上方 32px） → `WindowChrome(this, tb, resizable=true)`；connect close/minimize/maximizeToggled；`changeEvent(WindowStateChange)` 同步 `tb->setMaximizeButtonChecked`（Qt 5.15 无 `windowStateChanged` signal） |

## 3. 架构与契约

```
┌────────────────────────────────────────────┐
│  QDialog / QMainWindow                     │
│  ├─ WindowChrome (FramelessHelper*, ...)   │  ← 唯一接入点
│  │   └─ FramelessHelper (eventFilter)      │     拖拽 / 边缘缩放 / Aero Snap
│  ├─ TitleBar  (objectName=#EnerSentry…)    │  ← 32px 自绘，QSS 钩子
│  │   ├─ QLabel logo    (:/icons/app_logo)
│  │   ├─ QLabel title
│  │   ├─ QSpacerItem    (Expanding)
│  │   ├─ QPushButton min   (objectName=minBtn)
│  │   ├─ QPushButton max   (objectName=maxBtn)
│  │   └─ QPushButton close (objectName=closeBtn, hover 红底)
│  └─ 业务内容
└────────────────────────────────────────────┘
```

**FramelessHelper 核心 API**（切片 45.1）

| 成员 | 作用 |
|---|---|
| `FramelessHelper(target, dragRegion=TitleBar, parent=target)` | 构造即安装 eventFilter 接管 target+dragRegion |
| `setResizable(bool)` | 边缘缩放开关（Dialog=false / MainWindow=true） |
| `setMovable(bool)` | 拖拽开关（锁定屏等场景关） |
| `setSnapToMaximize(bool)` | Aero Snap：拖到屏幕顶部自动最大化 |
| `hitTestZone(QPoint)→int` | 0..7=8 边/角、8=none（单元测试用） |
| `zoneToEdges(int)→Qt::Edges` | 8 区→Qt 边组合（4→NW etc.） |
| signals: `moved / resized / snapMaximized / dragStarted / dragFinished` | 消费者可观测 |

**TitleBar 核心 API**

| 成员 | 作用 |
|---|---|
| `setTitle(QString)` / `setTitleIconPath(QString)` | 标题文字 + 标题图标（默认 `:/icons/app_logo.svg`） |
| `setMinimumVisible(bool)` / `setMaximumVisible(bool)` | Min/Max 按钮显隐（Dialog 经常只保留 close） |
| `setMaximizeButtonChecked(bool)` | 切图标（max 状态=window_restore，正常=window_maximize） |
| signals: `closeClicked / minimizeClicked / maximizeToggled(bool) / doubleClicked / dragPressed` | 消费者接信号 → `close() / showMinimized() / showMaximized()/showNormal()` |

**QSS 契约**（resources/qss/theme.qss 追加）

```qss
#EnerSentryTitleBar { background: #1c2127; border-bottom: 1px solid #2a3038; }
#EnerSentryTitleBar QLabel#titleLabel { color: #d8dee6; font-weight: 500; font-size: 13px; }
#EnerSentryTitleBar QPushButton { background: transparent; border: none; padding: 0;
    min/max-width: 46px; min/max-height: 32px; color: #d8dee6; }
#EnerSentryTitleBar QPushButton:hover { background: #2d3540; }
#EnerSentryTitleBar QPushButton#closeBtn:hover { background: #e94560; color: #fff; }
```

## 4. 关键设计决策

| # | 决策 | 理由 |
|---|---|---|
| 1 | Q_OBJECT eventFilter 接管而非子类化 QDialog | FramelessHelper 与具体窗体解耦；同一份代码同时挂给 Dialog + MainWindow |
| 2 | 拖拽/缩放用 Qt 5.15 原生 `startSystemMove/Resize`，offscreen 平台插件走兜底 | Windows 平台走 Aero 流畅；offscreen 测试可单测 hitTestZone/zoneToEdges |
| 3 | `setMenuWidget(TitleBar)` 而非"标题栏放 menubar 下" | 视觉上更合理（Logo + 标题贴近屏幕顶端），menubar 紧邻 TitleBar 无缝 |
| 4 | `changeEvent(WindowStateChange)` 同步 TitleBar 最大化图标 | Qt 5.15 无 `windowStateChanged` signal（Qt 6 才有），必须用 changeEvent |
| 5 | Dialog 模式 resizable=false | 防止误触发边缘缩放，登录/锁定/告警等小窗体保持固定尺寸 |
| 6 | 测试链 `resources.qrc` → `ens_tests` | 否则 `:/icons/window_*.svg` 在测试进程内 `QIcon isNull()`，case 全挂 |
| 7 | offscreen grab 前显式 `applyTheme(qApp) + processEvents()` | QSS 懒应用，offscreen 平台下 widget grab 不自动触发解析 |

## 5. 验证（量化指标）

| 维度 | 数据 |
|---|---|
| ctest | **343/343 全过**（5m 59s）—— 之前 325 → 343 = +18 |
| 新增 case | 7 FramelessHelper（hitTestZone 9 区 + 边界 20x20 + 状态 getter + null target）+ 6 TitleBar（构造/setTitle/setIconPath/可见性/信号/切图标）+ 5 chrome render（TitleBar pixmap/背景暗/close 非红/LoginDialog 380×272 + 顶部暗色/MainWindow 1280×800 + 顶部暗色） |
| 新增文件 | 6 个（3 对 .h/.cpp）+ 4 SVG + 3 测试文件 |
| 代码增量 | FramelessHelper≈170 行 + TitleBar≈190 行 + WindowChrome≈40 行 + LoginDialog 接入≈10 行 + MainWindow 接入≈15 行 + theme.qss +30 行 = **≈455 行** |
| 新增库依赖 | **0**（纯 Qt5 Gui 基础控件） |
| 现有测试破坏 | 0 |
| 暗色覆盖验收 | offscreen 平台 grab 像素采样：`TitleBar.top-left.red < 80 && .green < 80 && .blue < 80`（避免"全红"）；close 按钮区同条件；`MainWindow 顶部 (200, 16)` 同条件 |
| 验收 PNG | `build/vs2022-debug/login_chrome.png`（登录窗 380×272）+ `build/vs2022-debug/mainwindow_chrome.png`（主窗 1280×800）；均复制到 `outputs/s45p2_login_chrome.png` / `outputs/s45p3_mainwindow_chrome.png` |

## 6. 关键坑位（实测沉淀，纳入 MEMORY.md）

1. **Qt 5.15 vs Qt 6 API 差异**：`QMouseEvent::globalPosition()` 是 Qt 6；Qt 5.15 必须用 `globalPos()`。同：`windowStateChanged` signal 是 Qt 6；Qt 5.15 用 `changeEvent(WindowStateChange)`。
2. `Qt::Edges` 没有 `isNull()`，必须用 `!= Qt::Edges{}` 比较。
3. `QPointer<T>` 的 `operator T*` 要求 T 完整类型——`WindowChrome::helper()` 返回 `FramelessHelper*` 裸指针（不要用 `QPointer<T>` 的隐式转换）。
4. `QWidget::isVisible()` 受父链 visibility 影响——单测用 `isVisibleTo(parent)`。
5. **8x8 target 边界预期写错**：`qMin(borderWidth=6, size/2=4) = 4` 之后整个 8x8 widget 都在 border 上；改 20x20 验 `(10, 10) == 8`。
6. **catch2 v3 `INFO(<< x)` 要求 x 有 `operator<<`**——Qt 类型（QColor/QString）不直接支持；用 REQUIRE 失败自然报 expansion。
7. **QSS 懒应用**：offscreen 平台下 widget grab() 不会自动触发 QSS 解析，需 `applyTheme + processEvents()` 后再 grab。
8. **测试链 `resources.qrc` 是隐藏前置**——ens_tests 不链 rcc 时 `:/icons/*` SVG 全部 `QIcon::isNull()`，所有图标相关 case 全挂。

## 7. 变更面（与 V2R/P4R 同格式）

| 类型 | 文件 |
|---|---|
| 新增 .h | FramelessHelper.h / TitleBar.h / WindowChrome.h / test_frameless_helper.cpp / test_titlebar.cpp / test_chrome_render.cpp |
| 新增 .cpp | FramelessHelper.cpp / TitleBar.cpp / WindowChrome.cpp |
| 新增 SVG | window_minimize / window_maximize / window_restore / window_close.svg |
| 修改 | LoginDialog.h / LoginDialog.cpp / LoginDialog.ui（topMargin=0）/ MainWindow.h / MainWindow.cpp（构造时 Frameless + changeEvent）/ resources.qrc（4 alias）/ resources/qss/theme.qss（+30 行 TitleBar 样式）/ apps/ens_app/CMakeLists.txt（+3 源）/ tests/CMakeLists.txt（+3 test + resources.qrc） |

合计：+14 文件，~5 文件改。

## 8. 后续可复用建议

- **SessionLockDialog** / **CriticalAlarmDialog** / **About QMessageBox** 等"原生标题栏"小窗体可一行 `WindowChrome(dlg, tb, false)` 接入，标题栏样式自动一致。
- **弹窗动画 / 窗口圆角 / 阴影**未来扩展：QSS 加 `border-radius: 6px;` + `QGraphicsDropShadowEffect`，与 FramelessHelper 解耦，无须改本切片任何代码。
- **i18n 标题栏**：TitleBar 标题现为 QString，无国际化钩子；后续可加 `tr("EnerSentry Login")`。
