# EnerSentry 储能上位机系统 —— UI 详细设计说明书（UI-DD）

> **文档编号**：ENS-UI-DD-001  
> **版本**：V1.5  
> **日期**：2026-08-12  
> **状态**：正式发布（可工程落地）  
> **编制依据**：
> - 《EnerSentry-储能上位机系统-概要设计说明书 V1.5》(ENS-HLD-001)
> - 《EnerSentry-储能上位机系统-软件需求规格说明书（SRS）V1.1》(ENS-SRS-001)
> - 《EnerSentry-非功能保障设计说明 V1.2》(ENS-NFR-001)
> - 《EnerSentry-协议引擎设计说明 V1.0》(ENS-PEDS-001)
> - 《EnerSentry-通信接入设计说明 V1.5.3》(CADS)
> - 《EnerSentry-线程模型与并发设计专题报告 V1.0》(ENS-CONC-001)
> - 《EnerSentry-数据库设计说明书（DBDD）》(ENS-DBDD-001)
> - 《EnerSentry-接口控制文档-ICD_IDD_Design_Specification V1.14》(ENS-ICD-001)
> - 《EnerSentry-工业上位机实战项目蓝图 V2.0》
>
> **适用人员**：UI 软件工程师、Qt/C++ 高级开发工程师、前端交互设计师、测试工程师、技术评审人员

---

## 文档修订记录

| 版本 | 日期 | 修订人 | 修订内容 |
|------|------|--------|---------|
| V1.0 | 2026-08-12 | 资深 Qt/C++ UI 工程师 | 初始版本。基于 HLD V1.5 与全栈设计文档，输出可工程落地的 UI 详细设计，覆盖总体架构规范、7 大核心视图模块、核心 UI 类接口、渲染降采样与性能优化四章 |
| V1.1 | 2026-08-12 | 资深 Qt/C++ UI 工程师（Code Review） | §4.2 零拷贝优化：QVector 深拷贝改 std::swap；§4.3 新增 OpenGL 兼容性降级预案（工控机无独显自动回退软件渲染）；§1.2 新增 High DPI QSS 变量集中化 + 自适应布局约束；§3.1/附录新增界面状态持久化（QSettings）与国际化（i18n/tr()）补充 |
| V1.2 | 2026-08-12 | 资深 Qt/C++ UI 工程师（Code Review R2） | §4.2 隐藏通道数据溢出防护（Invisible Channel 早返回+清空 pendingSamples）；§2.1 TopoView 矢量线宽 High DPI 下 setCosmetic(true) 固定物理像素；§4.3 QStackedWidget 中 OpenGL Widget 隐藏/显示上下文丢失修复（showEvent 刷新）；§4.2 实时曲线用户交互竞用保护（m_userInteracting 标志位 + Pause/Pan 模式下跳过 replot 或追加后台历史 Array） |
| V1.3 | 2026-08-12 | 资深 Qt/C++ UI 工程师（Code Review R3） | §4.2 m_backlog 恢复实时前硬限清理策略（>2000 点先降采样再回放或提供"跳过历史恢复当前"轻量路径）；§3.2/§3.4 i18n 动态切换 QCustomPlot 轴标签刷新（changeEvent 手动 setLabel(tr()) + replot）；§2.1 L2 电芯级 100+ 极端渲染性能预案（禁止嵌套 QWidget/QLabel，改用 QGraphicsView 批量图元 或 QAbstractTableModel + 自定义 QItemDelegate 绘制） |
| V1.4 | 2026-08-12 | 资深 Qt/C++ UI 工程师（Code Review R4 编码微观优化） | §3.2 后台隐藏时 m_repaintTimer 智能挂起（hideEvent stop / showEvent start）；§2.1 电芯级 QItemDelegate 局部刷新 Rect 精准计算（emit dataChanged(index, index, {Qt::DisplayRole}) 避免全 View 重绘）；§4.2 std::swap 容量 Capacity 预留（初始化时 reserve(MAX_POINTS_PER_CHANNEL) 确保 swap 后高频 append 不触发 realloc） |
| V1.5 | 2026-08-12 | 资深 Qt/C++ UI 工程师（Code Review R5 锦上添花） | §3.2/§4.2 m_backlog 交互态 UI 状态显性化（Paused/Panning 时图表右上角半透明悬浮 Banner + 点击恢复实时）；§2.1 TopoView 静态背景图元 DeviceCoordinateCache 缓存策略（fitInView 复用 Pixmap 避免重绘大量路径）；§3 新增 i18n 枚举转换工具类 QStringUtils（formatAlarmLevel/formatSlaveHealth 等，内部 tr() 包裹，语言切换自动更新） |

---

## 目录

1. [界面总体架构与设计规范](#1-界面总体架构与设计规范)
   - 1.1 [总体布局规范（Top / Nav / Center / Bottom）](#11-总体布局规范top--nav--center--bottom)
   - 1.2 [视觉规范（QSS 暗色工业主题调色板）](#12-视觉规范qss-暗色工业主题调色板)
   - 1.3 [交互规范（DrillDownNavigator 三级钻取路由）](#13-交互规范drilldownnavigator-三级钻取路由)
2. [7 大核心视图模块 UI 详细设计](#2-7-大核心视图模块-ui-详细设计)
   - 2.1 [① 电站总览 OverviewWidget](#21-①-电站总览-overviewwidget)
   - 2.2 [② 实时曲线 RealtimeChartWidget](#22-②-实时曲线-realtimechartwidget)
   - 2.3 [③ 告警中心 AlarmCenterWidget](#23-③-告警中心-alarmcenterwidget)
   - 2.4 [④ 历史趋势 HistoryTrendWidget](#24-④-历史趋势-historytrendwidget)
   - 2.5 [⑤ 参数配置 ConfigWidget](#25-⑤-参数配置-configwidget)
   - 2.6 [⑥ 通信诊断 DiagWidget](#26-⑥-通信诊断-diagwidget)
   - 2.7 [⑦ SBO 控制台 SBOControlWidget](#27-⑦-sbo-控制台-sbocontrolwidget)
3. [核心 UI 类接口设计（Class API）](#3-核心-ui-类接口设计class-api)
   - 3.1 [MainWindow](#31-mainwindow)
   - 3.2 [RealtimeChartWidget](#32-realtimechartwidget)
   - 3.3 [SBOControlWidget](#33-sbocontrolwidget)
   - 3.4 [AlarmCenterWidget](#34-alarmcenterwidget)
   - 3.5 [i18n 枚举转换工具类 QStringUtils（V1.5）](#35-i18n-枚举转换工具类-qstringutilsv15)
4. [UI 性能优化与渲染降采样设计](#4-ui-性能优化与渲染降采样设计)
   - 4.1 [数据通路：ens::datahub → UI 渲染线程](#41-数据通路ensdatahub--ui-渲染线程)
   - 4.2 [QTimer 批量重绘与单通道 ≤2000 点降采样伪代码](#42-qtimer-批量重绘与单通道-2000-点降采样伪代码)
   - 4.3 [工程约束与验收指标](#43-工程约束与验收指标)
     - 4.3.1 [CMake 构建约束（ens::ui STATIC）](#431-cmake-构建约束ensui-static)
     - 4.3.2 [OpenGL 加速兼容性降级预案](#432-opengl-加速兼容性降级预案工控机无独显场景)
     - 4.3.3 [OpenGL Widget 在 QStackedWidget 隐藏/显示时的上下文丢失修复](#433-opengl-widget-在-qstackedwidget-隐藏显示时的上下文丢失修复)
   - 4.4 [High DPI 自适应与 QSS 变量集中化](#44-high-dpi-自适应与qss-变量集中化)
   - 4.5 [界面状态持久化与国际化（i18n）补充](#45-界面状态持久化与国际化i18n-补充)

---

## 1. 界面总体架构与设计规范

### 1.1 总体布局规范（Top / Nav / Center / Bottom）

EnerSentry HMI 采用 **三段式主框架 + 可切换中央视图** 的布局（对应 SRS UI-01 暗色工业主题、UI-05 1920~3840 自适应、UI-07 双屏扩展）。整个应用为单 `QMainWindow` 骨架，所有业务视图均作为 `QStackedWidget` 中的 Page 挂载，由 `DrillDownNavigator` 统一路由。

> **启动顺序（FR-AUTH-01 / ENS-LLD-500 §7）**：应用启动后**先弹出 `LoginDialog` 模态登录屏**（位于 `src/ui/`，由 `main.cpp` 以 `exec()` 阻塞），鉴权成功后才构造并显示 `MainWindow`；会话超时（FR-AUTH-05）由 `SessionLockDialog` 重新锁定，采集/通信后台不中断。本说明书其余章节均描述“已登录态”的主框架与视图。

```
┌────────────────────────────────────────────────────────────────────────┐
│  TOP BAR  全局状态栏  │ 站名 / 系统时钟 / 角色(RBAC) / 全局健康灯 / SBO 锁态 │  ← 高 48px
├──────────┬─────────────────────────────────────────────────────────────┤
│  NAV BAR │                                                             │
│  左侧导航 │                      CENTER  中央主显示区                  │  ← 主工作区
│  (图标+   │    (QStackedWidget 切换：总览/曲线/告警/趋势/配置/诊断/SBO)  │
│   文字)   │                                                             │
│  高 64px  │                                                             │
├──────────┴─────────────────────────────────────────────────────────────┤
│  BOTTOM BAR  通信状态(多链路 LED)  │  最新告警滚动条  │  CPU/内存/帧率    │  ← 高 36px
└────────────────────────────────────────────────────────────────────────┘
```

| 区域 | 控件 | 职责 | 绑定信号/数据 |
|------|------|------|--------------|
| **Top Bar** | `GlobalStatusBar` | 显示站名、NTP 系统时钟、当前登录角色（操作员/工程师/管理员）、全站设备健康汇总灯、SBO 设备锁占用数（来自 `DeviceSboGuard`） | `ISBOManager::armedAcquired/armedReleased`、`IDataAccess::globalHealthChanged` |
| **Nav Bar** | `NavDock`（左侧竖排 `QToolButton` + `QListWidget`） | 7 大模块入口；支持键盘快捷键（F1~F7）与权限灰显（RBAC）；当前页高亮 | `DrillDownNavigator::navigateTo(PageId)` |
| **Center** | `QStackedWidget` + `QScrollArea` | 承载 7 个核心视图 Page；切换时执行页面切换动画（1.3.2） | 各 Widget 的 `refresh()` / `bindBusiness()` |
| **Bottom Bar** | `CommStatusBar` + `AlarmTicker` + `PerfMeter` | 多链路通信 LED（来自 `ChannelStats`）、最新一条告警滚动提示、实时 CPU/内存/帧率自监控 | `IChannel::connectionChanged`、`IAlarmEngine::alarmTriggered`、`PerfMeter::tick` |

**双屏扩展（UI-07）**：主屏渲染 Center 主视图；副屏（可选）通过独立 `QMdiArea` / 第二 `QMainWindow` 镜像"电站总览 + 实时曲线"只读视图，由 `ScreenManager` 在启动时探测 `QGuiApplication::screens()` 数量决定布局。

### 1.2 视觉规范（QSS 暗色工业主题调色板）

整套主题由 `ThemePalette`（单例）集中管理，QSS 以变量宏方式引用（见 §3.1 `ThemePalette`），严禁在业务代码内硬编码颜色。

#### 1.2.1 核心调色板（QSS 变量表）

| 变量宏 | 值（十六进制） | 用途 | 对应文档 |
|--------|--------------|------|---------|
| `--bg-base` | `#1a1a2e` | 应用主背景（暗色基底） | UI-01 |
| `--bg-panel` | `#16213e` | 面板 / 卡片 / 控件容器背景 | — |
| `--bg-elevated` | `#0f3460` | 浮层 / 弹窗 / 选中态背景 | — |
| `--border` | `#2a3a5e` | 控件描边、分隔线 | — |
| `--text-primary` | `#e6e9f0` | 主文字（浅色） | UI-01 暗色浅字 |
| `--text-secondary` | `#9aa7c7` | 次级文字 / 单位 / 占位 | — |
| `--accent` | `#00d4ff` | 主强调色（聚焦、链接、主按钮） | — |
| `--accent-hover` | `#33e0ff` | 强调色 hover | — |
| **告警三级色** | | | UI-02 |
| `--alarm-info` | `#4aa3ff` | **Info 级（蓝色）**：普通提示、状态变化 | NFR / SRS UI-02 |
| `--alarm-warning` | `#ffcc00` | **Warning 级（黄色）**：越限预警、需关注 | SRS UI-02 黄=一般 |
| `--alarm-critical` | `#ff3b3b` | **Critical 级（红色）**：严重越限、急停 | SRS UI-02 红=严重 |
| **设备状态色** | | | UI-02 |
| `--status-normal` | `#3ad29f` | 设备/测点正常（绿色） | SRS UI-02 绿=正常 |
| `--status-offline` | `#6b7280` | 离线 / 未采集（灰） | — |
| `--status-busy` | `#ff9f43` | 采集忙 / 通信降级（橙） | — |

> **色彩语义铁律（SRS UI-02）**：严重=红、一般=黄、正常=绿。告警三级映射固定为 `AlarmLevel::Info→蓝 / Warning→黄 / Critical→红`，不可随主题切换而漂移；色盲友好场景下叠加图标（▲/⚠/✖）与文字标签，不依赖纯颜色传达语义。

#### 1.2.2 字体与字号规范

| 用途 | 字体族 | 字号 | 字重 |
|------|--------|------|------|
| 数字 / 遥测值（等宽数字） | `Source Han Sans SC` + `Roboto Mono`（回退 `Consolas`） | 14~22px | Medium |
| 标题（Panel/Card 标题） | `Source Han Sans SC` | 14px | Bold |
| 正文 / 列表 | `Source Han Sans SC` | 12~13px | Regular |
| 告警弹窗正文 | `Source Han Sans SC` | 14px | Medium |

- 工程单位（UI-03）严格跟随测点 `Point::unit`（如 `℃` / `%` / `V` / `A` / `kW` / `kWh`），不做隐式换算；大数据采用千分位或工程计数（如 `12.34 kWh`、`1.2 kV`）。
- 高 DPI：`QApplication::setAttribute(Qt::AA_EnableHighDpiScaling)` + `setFont` 以 `pt` 逻辑单位，保障 1920×1080 → 3840×2160 4K 下无模糊。

#### 1.2.3 统一控件样式规则（QSS 片段，V1.1 含 DPI 变量）

> **V1.1 改进**：所有尺寸值通过 `--dpi-N` 变量引用（由 `ThemePalette::injectDpiVariables()` 在启动时根据 `logicalDotsPerInch()` 注入实值），避免 1080P→4K 缩放下布局变形。详见 §4.4。

```css
/* theme/ens_dark.qss（节选，变量由 ThemePalette 注入） */
QWidget#Panel {
    background: var(--bg-panel);
    border: var(--border-w) solid var(--border);
    border-radius: var(--radius-md);          /* --dpi-6 × scale */
}
QPushButton#Primary {
    background: var(--accent);
    color: #06121f;
    border-radius: var(--radius-sm);         /* --dpi-4 × scale */
    padding: var(--pad-md);                  /* --dpi-10 × scale 上下, --dpi-14 左右 */
    font-weight: 600;
}
QPushButton#Primary:hover { background: var(--accent-hover); }
QPushButton#Primary:disabled { background: #2a3a5e; color: #6b7280; }
QPushButton#Danger { background: var(--alarm-critical); color: #fff; }  /* SBO 执行/急停 */
QTableWidget, QTreeView {
    background: var(--bg-panel);
    gridline-color: var(--border);
    selection-background-color: var(--bg-elevated);
}
QScrollBar:vertical { background: var(--bg-base); width: var(--scrollbar-w); }
```

### 1.3 交互规范（DrillDownNavigator 三级钻取路由）

#### 1.3.1 三级钻取模型（对应概要架构层级）

电池层级为 **储能电站 → 电池舱 Container ×N → 电池簇 Rack ×M → 电池包 Pack ×K → 电芯 Cell ×100+**（蓝图）。UI 钻取遵循三级粒度：

| 钻取层级 | 视图 | 内容 | 进入条件 |
|---------|------|------|---------|
| **L0 站级** | 电站总览 | 拓扑图 + 全站指标卡（SOC/SOH/功率/温度极值） | 默认首页 |
| **L1 舱/簇级** | 总览下钻 / 曲线 / 告警过滤 | 单 Container/Rack 的测点聚合、曲线、告警 | 点击拓扑节点（< 200ms 切换） |
| **L2 包/点级** | 曲线聚焦 / 测点详情 | 单 Pack 或单 Point 历史曲线、实时值、黑匣子 | 行点击 / 曲线 legend 选取 |

`DrillDownNavigator` 维护 **钻取栈（DrillStack）**，支持面包屑（Breadcrumb）回退与 `Esc` 逐级返回；任意层级切换必须满足 **< 200ms 响应**（SRS NFR-PERF-09、Q-10 冷启动 < 5s）。

#### 1.3.2 页面切换动画与防抖节流（Anti-Flood Throttling）

- **切换动画**：`QStackedWidget` 切换使用 `QPropertyAnimation` 对 `widget->opacity` 做 120ms 淡入（`QEasingCurve::OutCubic`），不阻塞事件循环；动画期间禁止再次触发切换。
- **防抖节流（强制）**：`DrillDownNavigator::navigateTo(PageId)` 实现 **200ms 输入节流 + 状态去重**：
  - 同一 `PageId` 连续触发（如重复点击导航）**仅首次生效**，后续直接 return；
  - 高频导航事件（如滚轮/键盘连按）经 `QTimer::singleShot(120ms)` 合并，避免 `QStackedWidget` 抖动与 Widget 重复 `bindBusiness()`。
- **懒加载**：非首页 Page 首次进入时才 `bindBusiness()` 订阅业务信号，离开不 `disconnect`（保留订阅），降低切换开销。

---

## 2. 7 大核心视图模块 UI 详细设计

> **模块隔离铁律（工程约束）**：`ens::ui` 仅依赖 `ens::business` 与 `qcustomplot`。**严禁**在 UI 代码中 `new QSerialPort` / `QTcpSocket` / 直接调用 `IChannel` 读写；所有实时数据经 `IDataAccess` / `DataHub` 订阅，所有控制经 `ISBOManager` 下发。UI 线程**绝不**在工作线程上下文执行 `replot()` 或触碰 `QWidget` 的子对象——跨线程一律 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`。

### 2.1 ① 电站总览 OverviewWidget

**职责**：L0 站级态势感知（FR-OV-01~07）。拓扑图 + 全站指标卡 + 三级钻取入口 + 设备健康汇总。

#### 2.1.1 类结构

```
OverviewWidget (QWidget)
├── TopoView          (QGraphicsView)   拓扑画布（电站→舱→簇）
├── MetricCardPanel   (QWidget)         指标卡容器（SOC/SOH/有功/无功/温度）
├── HealthSummary     (QWidget)         全站设备健康汇总（正常/降级/离线计数）
└── Breadcrumb        (QWidget)         钻取面包屑
```

#### 2.1.2 Widget Tree

```
OverviewWidget
└── QHBoxLayout
    ├── TopoView (QGraphicsView)
    │   └── QGraphicsScene
    │       ├── StationNode (自定义 QGraphicsItem)
    │       ├── ContainerNode × N   → 点击 drillDown(L1)
    │       │   └── RackNode × M     → 点击 drillDown(L2)
    │       └── LinkEdge（通信链路连线，颜色随 ChannelStats）
    └── QVBoxLayout (右栏)
        ├── MetricCardPanel
        │   ├── MetricCard[总 SOC%] / [总 SOH%] / [有功 kW] / [无功 kVar] / [最高温度 ℃]
        └── HealthSummary
            ├── QLabel(正常×N) / QLabel(降级×N) / QLabel(离线×N)
            └── Breadcrumb（站 / 舱 / 簇 三级）
```

#### 2.1.3 信号 / 槽

| 方向 | 信号 / 槽 | 说明 |
|------|-----------|------|
| 业务→UI | `IDataAccess::stationMetricUpdated(const StationMetric&)` | 指标卡刷新（QueuedConnection） |
| 业务→UI | `IDataAccess::deviceHealthChanged(uint32_t devId, SlaveHealth)` | 拓扑节点颜色更新 |
| UI→业务 | `OverviewWidget::drillDown(const DrillKey&)` | 经 `DrillDownNavigator` 路由到 L1/L2 |
| UI→UI | `Breadcrumb::navigateUp()` | 面包屑回退一级 |

#### 2.1.4 交互逻辑

1. 启动后默认呈现 L0 拓扑；`TopoView` 采用固定布局 + 自适应缩放（`fitInView`），不随分辨率拉伸变形。通信链路连线（`LinkEdge`）与节点描边统一使用 **Cosmetic Pen**（`setCosmetic(true)`），在 1080P→4K 缩放下保持固定物理像素线宽，避免线宽随视图缩放跳变（V1.2，详见 §4.4.3④）。
2. 点击 `ContainerNode` → `drillDown(Container)`：右侧指标卡切换为该舱聚合值，面包屑追加"舱名"，**< 200ms**（数据来自 L1 聚合缓存，非实时重算）。
3. 设备健康灯颜色映射：`SlaveHealth::HEALTHY→绿` / `DEGRADED→橙` / `ISOLATED→灰` / 离线→灰；Critical 关联告警时节点闪烁红边（引用 `AlarmEngine` 状态）。
4. 双击节点可跨模块跳转：双击 PCS 节点 → `DrillDownNavigator::navigateTo(Page_SBO)` 并预选该设备。
5. **V1.3 L2 电芯级（Cell Level 100+）极端渲染性能预案**：
   - **场景**：在 L2 包/点级或 Overview 下钻到电芯列表时，单页面可能同时呈现 100~300 个电芯的电压/温度方块。若采用嵌套 `QWidget` / `QLabel` 逐个实例化，将导致：
     - 布局引擎 O(N) 计算量爆炸（300+ 子 Widget 的 `sizeHint`/`event` 级联传播）；
     - 每次数据刷新触发 300+ 次 `setText()` + `update()`，主线程 CPU 飙升；
     - 内存占用线性膨胀（每个 QLabel 约 1~2KB 元数据），极端场景 > 1MB 仅用于标签。
   - **禁止方案**：**严禁在电芯级视图使用嵌套 QWidget/QLabel 逐格渲染**。
   - **推荐方案 A（矢量图元批量绘制）**：使用 `QGraphicsView` + 自定义 `QGraphicsItem` 子类（如 `CellGridItem`），在单个 `paint()` 中批量绘制所有电芯色块与数值文本（类似 `QStaticText` 批量缓存）。所有电芯共享一个 `QGraphicsScene`，`QGraphicsView` 的 `viewport` 做整体 `update`，避免 N 次独立 `paintEvent`。
   - **推荐方案 B（模型-委托绘制）**：使用 `QTableView` / `QAbstractTableModel` + 自定义 `QItemDelegate`（或 `QStyledItemDelegate` 重写 `paint()`），在 Delegate 的 `paint()` 中直接用 `QPainter` 绘制电芯色块+数值（无需中间 QWidget）。数据刷新仅调用 `model->setData()` 触发单次 `dataChanged` 信号 → View 统一重绘可见区域。
   - **V1.4 方案 B 局部刷新优化**：当仅单个/少量电芯数据更新时，`dataChanged` 应**精准发射**变更的 index 范围与角色列表，避免触发全 View 重绘：
     ```cpp
     // 仅更新第 row 行、col 列的电芯数值——View 只重绘该格 Rect
     emit dataChanged(model->index(row, col), model->index(row, col),
                      {Qt::DisplayRole, Qt::BackgroundRole});
     // 若批量更新多行但非全部，使用 beginResetModel()/endResetModel() 会
     // 导致 View 完全重建（重算 sizeHint/布局），应改用上述逐格 dataChanged。
     ```
     对于 100+ 电芯的 10Hz~100ms 更新频率场景，**严禁使用 `beginResetModel()`/`endResetModel()`**（每次调用导致整个 TableView 重新计算所有行的 height/width + 全量 repaint），必须用 `dataChanged(index, index, roles)` 做局部脏区标记。
   - **选择依据**：若电芯需支持独立点击/悬浮 Tooltip → 方案 B（`QTableView` 天然支持行/列选择与 `indexAt(pos)`）；若需自由排列（非严格网格）或缩放平移交互 → 方案 A（`QGraphicsView` 场景坐标更灵活）。
6. **V1.5 TopoView 静态背景图元缓存策略**：
   - **场景**：`OverviewWidget::TopoView` 的 `QGraphicsScene` 中包含大量静态背景图元（如变压器外形轮廓、电池舱/PCS 轮廓、母线连线等），这些图元的 `paint()` 路径复杂（多段 Bezier / 多边形填充），且在窗口 resize 触发 `fitInView()` 缩放时会被重复绘制。
   - **问题**：每次 `fitInView()` 或视图变换时，`QGraphicsView` 默认对所有可见 Item 重新调用 `paint()`，导致 CPU 在重绘不变背景上浪费大量时间（尤其 4K 分辨率下路径光栅化开销显著）。
   - **方案**：对**不常变化的静态背景图元**显式启用 **DeviceCoordinateCache**：
     ```cpp
     // ui/TopoSceneBuilder.cpp —— V1.5 静态图标缓存
     QGraphicsItem* transformerOutline = new TransformerOutlineItem();
     transformerOutline->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
     // DeviceCoordinateCache：Qt 将 item 的 paint() 结果缓存为 Pixmap（设备坐标，即像素），
     //   后续 fitInView()/scale() 变换时直接复用缓存的 Pixmap，无需重新执行 paint() 路径。
     // 适用条件：item 内容不频繁变化（静态轮廓、固定图标）；
     //           不适用：实时变化的动态状态灯/数值文本（用 NoCache 或 default）。

     // 动态状态灯（如通信 LED 呼吸闪烁）保持默认不缓存——每帧需重绘最新颜色
     QGraphicsItem* statusLed = new StatusLedItem();
     statusLed->setCacheMode(QGraphicsItem::NoCache);  // 显式不缓存（默认亦可）
     ```
   - **效果**：`fitInView()` 触发时，静态背景从"逐路径光栅化"降为"Pixmap 缩放+合成"，CPU 开销降低约 **40%~60%**（取决于场景中静态图元占比）。首次 `paint()` 仍有一次性开销（构建缓存 Pixmap），后续变换几乎零开销。

### 2.2 ② 实时曲线 RealtimeChartWidget

**职责**：多通道实时曲线（FR-RT-01~08）。QCustomPlot 多通道、legend 控制、缩放/平移、30/60Hz 定时器绑定批量重绘。**严禁数据到达即 replot()**（NFR-PERF-13 / ADR-NFR-03）。

#### 2.2.1 类结构

```
RealtimeChartWidget (QWidget)
├── QCustomPlot*            m_plot          (OpenGL 加速)
├── ChannelListPanel        (QWidget)       legend/通道开关
├── Toolbar                 (QWidget)       缩放/平移/游标/快照
└── QTimer*                 m_repaintTimer  (30Hz/60Hz 批处理重绘)
```

#### 2.2.2 Widget Tree

```
RealtimeChartWidget
└── QHBoxLayout
    ├── ChannelListPanel (QVBoxLayout)
    │   ├── QCheckBox[通道1 温度] ...  → 控制 graph 显隐
    │   └── QPushButton[全选/全不选]
    └── QVBoxLayout
        ├── Toolbar
        │   ├── QToolButton[缩放] / [平移] / [复位] / [游标]
        │   └── QComboBox[时间窗 1m/5m/30m]
        └── QCustomPlot (m_plot, OpenGL 由 ThemePalette::tryEnableOpenGL() 探测)
```

#### 2.2.3 信号 / 槽

| 方向 | 信号 / 槽 | 说明 |
|------|-----------|------|
| 业务→UI | `IDataAccess::realtimeSampleReady(uint32_t pointId, const QCPGraphData&)` | **仅入队缓冲，不重绘** |
| UI 内部 | `QTimer::timeout → onBatchRepaint()` | 唯一重绘入口（§4.2） |
| UI→UI | `ChannelListPanel::channelToggled(pointId, bool)` → `onChannelToggled()` | `graph->setVisible()`；**隐藏时清空 `pendingSamples`（V1.2 防回灌）**，并 `replot(rpQueuedReplot)` |
| UI→业务 | `Toolbar::snapshotRequested()` | 请求 `IDataAccess::exportCurrentWindow()` |

#### 2.2.4 交互逻辑

1. 数据到达经 `realtimeSampleReady` 写入各通道**待绘制环形缓冲**（`pendingSamples`），**绝不调用 replot()**（强制约束，详见 §4）。
2. `m_repaintTimer`（默认 30Hz，`Qt::PreciseTimer`）触发 `onBatchRepaint()`：执行降采样 → `setData()` → `replot(rpQueuedReplot)`（同帧合并）。
3. legend/通道开关即时显隐对应 `QCPGraph`；缩放/平移使用 QCustomPlot 原生 `QCPAxisRect` 交互（`setInteraction`）；时间窗切换重置 X 轴范围。
4. 单通道点数 > 2000 或像素宽 > 1920 时自动降采样（Min-Max / LTTB 自适应，§4.2）。
5. **V1.2 交互态竞用保护**：用户拖拽 / 平移 X 轴（Pause/Pan 模式）浏览历史时，进入 `m_userInteracting` 交互态——`onBatchRepaint()` 不再 `setData`/重置视图，仅将新样本追加至后台 `m_backlog`（不丢数据）；松手或点击"恢复实时"（`setRenderMode(Live)`）时回放 `m_backlog` 并复位 X 轴滚动窗口。隐藏通道（`channelToggled(false)`）立即清空 `pendingSamples`，避免恢复可见瞬间回灌旧数据（§4.2）。
6. **V1.2 GL 上下文**：本页在 `QStackedWidget` 中隐藏/显示时，`showEvent` 触发 `viewport()->update()` + `replot()` 刷新重建后的 OpenGL 上下文，规避切回黑屏/闪烁（§4.3.3）。
7. **V1.5 交互态显性化 Banner**：当进入 Paused/Panning 模式时，图表右上角自动浮现半透明悬浮 Banner（`m_pauseBanner`，QLabel），显示"⏸ 实时滚动已暂停 — [点击恢复实时]"或"✋ 平移浏览中 — [点击恢复实时]"（文本经 `tr()` 包裹支持 i18n）；点击 Banner 或调用 `setRenderMode(Live)` 时隐藏。防止操作员离开工位后误判"数据卡死/通信中断"（详见 §3.2 类声明与 §4.2 setRenderMode 实现）。

### 2.3 ③ 告警中心 AlarmCenterWidget

**职责**：告警查询、声光弹窗、确认/清除、黑匣子回放入口（FR-AL-01~13）。

#### 2.3.1 类结构

```
AlarmCenterWidget (QWidget)
├── QTableView*          m_table          (自定义模型 AlarmTableModel)
├── AlarmItemDelegate*   m_delegate        (三级配色+图标绘制)
├── FilterBar            (QWidget)         级别/设备/时间过滤
├── AlarmPopup           (QWidget, 置顶)   声光弹窗（Critical/新告警）
└── Toolbar              (确认/清除/回放)
```

#### 2.3.2 Widget Tree

```
AlarmCenterWidget
└── QVBoxLayout
    ├── FilterBar
    │   ├── QComboBox[级别 Info/Warning/Critical]
    │   ├── QLineEdit[设备过滤] / QDateEdit[时间]
    ├── QTableView (m_table)
    │   └── AlarmItemDelegate (按 alarmLevel 着色行/图标)
    └── Toolbar
        ├── QPushButton[确认选中] / [全部确认] / [清除恢复] / [黑匣子回放]
```

#### 2.3.3 信号 / 槽

| 方向 | 信号 / 槽 | 说明 |
|------|-----------|------|
| 业务→UI | `IAlarmEngine::alarmTriggered(const AlarmRecord&)` | 入模型 + **Critical 触发声光弹窗** |
| 业务→UI | `IAlarmEngine::alarmRecovered(uint64_t)` / `alarmAcknowledged(uint64_t)` | 行状态更新 |
| 业务→UI | `IAlarmEngine::blackBoxRequested(uint32_t, uint64_t)` | 自动打开回放（跳 HistoryTrendWidget 定位 ±30s） |
| UI→业务 | `AlarmCenterWidget::acknowledge(const QVector<uint64_t>&, user)` | `IAlarmEngine::acknowledgeAlarms()` (FR-AL-08/10) |
| UI→业务 | `AlarmCenterWidget::clearRecovered()` | 清除已恢复告警（FR-AL-11） |
| UI→UI | `AlarmItemDelegate::replayRequested(alarmId)` | `DrillDownNavigator::navigateTo(Page_History)` + 定位 |

#### 2.3.4 交互逻辑

1. **声光弹窗**：`alarmTriggered` 经 QueuedConnection 投递；`Critical` 级弹出置顶 `AlarmPopup`（红边 + 闪烁 + 蜂鸣 `QSoundEffect`），**非 Critical 仅入列表 + 顶栏滚动提示**（FR-AL-03/07，避免打扰）。
2. **ItemDelegate** 按 `AlarmLevel` 绘制行背景/左侧色条/图标：Info 蓝 ▲ / Warning 黄 ⚠ / Critical 红 ✖；`AlarmStatus` 决定删除线（Recovered）或加粗（Active）。
3. **一键确认/清除**：批量 `acknowledgeAlarms(ids, user)`（记录 `confirmUser`，FR-AL-08）；"清除恢复"仅移除 `Recovered` 行。
4. **黑匣子回放入口**：点击某 Critical 告警行的"回放" → 跳历史趋势页，X 轴定位到告警时刻 ±30s（引用 L1 黑匣子 ±30s 数据）。

### 2.4 ④ 历史趋势 HistoryTrendWidget

**职责**：跨月 / 多粒度历史查询与对比、导出（FR-HT-01~08）。

#### 2.4.1 类结构

```
HistoryTrendWidget (QWidget)
├── TimeRangePicker   (QWidget)   起止时间 + 粒度(1s/5s/1min)
├── QCustomPlot*      m_plot      双 Y 轴对比
├── ChannelComparePanel (QWidget) 多通道叠加
└── ExportDialog      (QDialog)   CSV/Excel 导出
```

#### 2.4.2 Widget Tree

```
HistoryTrendWidget
└── QVBoxLayout
    ├── TimeRangePicker
    │   ├── QDateTimeEdit[起] / [止]  (支持跨月)
    │   └── QComboBox[粒度 1s/5s/1min] (引用 DBDD 降采样表)
    ├── ChannelComparePanel
    │   └── QListWidget[叠加通道] + QCheckBox[右轴]
    ├── QCustomPlot (m_plot, 左轴 QCPAxis / 右轴 QCPAxis)
    └── ExportDialog (QDialog)
        ├── QComboBox[CSV/Excel] / QPushButton[导出]
```

#### 2.4.3 信号 / 槽

| 方向 | 信号 / 槽 | 说明 |
|------|-----------|------|
| UI→业务 | `HistoryTrendWidget::queryHistory(req)` | `IDataAccess::beginQuery(HistoryQuery)`（跨月 ATTACH + 月度路由，Q-08/Q-09） |
| 业务→UI | `IDataAccess::historyResultReady(const HistoryPage&)` | 填充 `QCPGraphData` 并 `replot()` |
| UI→业务 | `ExportDialog::exportRequested(fmt, range)` | `IDataAccess::exportHistory()`（FR-HT-07） |

#### 2.4.4 交互逻辑

1. **时间选择器**：支持起止跨自然月；提交查询时 `IDataAccess` 自动按 `history_1s_YYYYMM` 月度表路由 + `ATTACH` + `UNION ALL`（DBDD），UI 仅传时间区间，不感知分表。
2. **粒度选择**：1s/5s/1min 对应 L2 已降采样表（`history_1s_*` / `history_5s_*` / `history_1m_*`），切换粒度即切换查询表，避免实时聚合。
3. **双 Y 轴对比**：勾选"右轴"的通道绘制到 `m_plot->yAxis2`，适合量纲差异大的测点（如温度 vs 功率）；最多叠加 8 通道（防过载）。
4. **导出**：`ExportDialog` 经 `IDataAccess` 流式导出，避免主线程卡顿；导出进度用 `QProgressDialog`。
5. **V1.3 i18n 轴标签刷新**：历史趋势页的 `QCustomPlot` 双 Y 轴标签（如"时间 (s)"、"温度 (℃)"、"功率 (kW)"）不继承 `QWidget`，无法自动响应 `changeEvent(LanguageChange)`。必须在 `HistoryTrendWidget` 中显式重写 `changeEvent`，手动调用 `m_plot->xAxis->setLabel(tr("时间 (s)"))` / `yAxis->setLabel(tr(...))` 并 `replot()`（模式同 §3.2 `RealtimeChartWidget::changeEvent`）。

### 2.5 ⑤ 参数配置 ConfigWidget

**职责**：点表与阈值热加载、树形编辑与校验（FR-CFG-01~10）。

#### 2.5.1 类结构

```
ConfigWidget (QWidget)
├── PointTreeView    (QTreeView)    link→slave→point 树
├── PointTableEditor (QWidget)      选中点的字段编辑（寄存器/字节序/系数/周期）
├── ThresholdEditor  (QWidget)       告警阈值编辑（引用 AlarmRule）
└── Toolbar          (加载/校验/热加载)
```

#### 2.5.2 Widget Tree

```
ConfigWidget
└── QHBoxLayout
    ├── PointTreeView (QTreeView, 模型 PointTableModel)
    │   └── 根[链路 linkId] → [从站 slaveAddress] → [测点 PointTableEntry]
    └── QVBoxLayout
        ├── PointTableEditor (表单: register_addr/byte_order/scale_factor/poll_interval_ms/unit)
        ├── ThresholdEditor (表单: 上限/下限/迟滞/延时确认/同源抑制)
        └── Toolbar
            ├── QPushButton[校验] / [热加载] / [导出 JSON]
```

#### 2.5.3 信号 / 槽

| 方向 | 信号 / 槽 | 说明 |
|------|-----------|------|
| UI→业务 | `ConfigWidget::loadPointTable(path)` | `ConfigManager::loadPointTable("config/pointtable.json")` → UPSERT `point_table`（FR-CFG-06） |
| UI→业务 | `ConfigWidget::reloadRules(rules)` | `IAlarmEngine::reloadRules()`（阈值热加载，FR-CFG-06/10） |
| UI→UI | `PointTreeView::currentChanged(entry)` | 填充编辑器表单 |
| 业务→UI | `IAlarmEngine::rulesReloaded()` | 提示热加载成功 |

#### 2.5.4 交互逻辑

1. **树形浏览**：以 `point_table`（`register_addr`, `byte_order` 0~3, `scale_factor`, `poll_interval_ms`，见 DBDD）为模型；树按 `link_id → slave_address → register_addr` 组织。
2. **编辑与校验**：`byte_order` 限制 0~3（`chk_byteorder`），`poll_interval_ms` ≥ 50ms；保存前 `validate()` 校验约束，非法项红框 + tooltip，禁止提交。
3. **热加载**：点"热加载" → `ConfigManager::loadPointTable()` 重新解析 JSON → `IDataAccess::getPointTable()` 刷新 → `IAlarmEngine::reloadRules()` 热更阈值；**无需重启进程**（FR-CFG-06，对应 Q-10 冷启动无关）。
4. **权限**：阈值编辑需工程师/管理员角色（RBAC），操作员仅可读。

### 2.6 ⑥ 通信诊断 DiagWidget

**职责**：多链路状态、实时十六进制报文捕获、通信质量百分比（FR-DG-01~06）。

#### 2.6.1 类结构

```
DiagWidget (QWidget)
├── LinkStatusGrid   (QWidget)    多链路 LED 矩阵
├── HexCaptureView   (QPlainTextEdit)  实时十六进制报文
├── QualityBarChart  (QCustomPlot)     通信质量% 柱状图
└── Toolbar          (开始/暂停捕获/清空)
```

#### 2.6.2 Widget Tree

```
DiagWidget
└── QVBoxLayout
    ├── LinkStatusGrid (QGridLayout)
    │   └── LinkLed × N (颜色随 ChannelStats: 优/一般/异常)
    ├── QSplitter
    │   ├── HexCaptureView (QPlainTextEdit, 只读, 等宽字体)
    │   └── QualityBarChart (QCustomPlot, 每链路一根柱)
    └── Toolbar [开始捕获][暂停][清空]
```

#### 2.6.3 信号 / 槽

| 方向 | 信号 / 槽 | 说明 |
|------|-----------|------|
| 业务→UI | `IChannel::connectionChanged(bool)` / `ChannelStats` 推送 | LED + 质量柱刷新（QueuedConnection） |
| 业务→UI | `IDataAccess::hexFrameCaptured(linkId, dir, QByteArray)` | 追加到 `HexCaptureView`（**节流批追加**，防刷新风暴） |
| UI→业务 | `DiagWidget::setCaptureEnabled(bool)` | 控制诊断订阅开关（FR-DG-02） |

#### 2.6.4 交互逻辑

1. **链路 LED**：每链路一盏，颜色随 `ChannelStats` 质量等级（优秀绿 / 一般黄 / 异常红 / 断开灰），Hover 显示丢帧率、超时计数。
2. **十六进制捕获**：报文经业务层诊断模块缓冲后**批量（~10Hz）**推到 UI 追加，单行格式 `HH:MM:SS.mmm [LINK1][TX] 01 03 00 00 00 0A C5 CD`；超长自动滚动截断（RingBuffer 限长 5000 行）。
3. **质量柱状图**：每链路通信质量%（60s 滑动窗口，来自 `ChannelStats`），`QCustomPlot` 柱状图 + 阈值线（< 90% 黄，< 70% 红）。

### 2.7 ⑦ SBO 控制台 SBOControlWidget

**职责**：Select-Armed-Operate 双重确认、设备级逻辑锁 UI 状态响应（FR-CTRL-01~07）。

#### 2.7.1 类结构

```
SBOControlWidget (QWidget)
├── DeviceTree       (QTreeView)   可操作设备/命令（SboCommand）
├── SboFlowPanel     (QWidget)     Select→Armed→Operate 状态机视图
├── ArmedCountdownBar(QProgressBar) Armed 5s 倒计时（急停 3s）
└── Toolbar          (Select/Arm/Cancel/Operate 按钮，权限灰显)
```

#### 2.7.2 Widget Tree

```
SBOControlWidget
└── QHBoxLayout
    ├── DeviceTree (QTreeView, 模型 SboCommandModel)
    │   └── 设备[PCS#1] → 命令[排风 0x1000][液冷 0x2000]
    └── QVBoxLayout
        ├── SboFlowPanel (阶段指示: Select/Armed/Operate/Executed)
        ├── ArmedCountdownBar (QProgressBar, 5s 倒计时)
        └── Toolbar
            ├── QPushButton[Select] / [Operate] / [Cancel]
            └── QPushButton[Danger: 急停] (3s 倒计时更短)
```

#### 2.7.3 信号 / 槽

| 方向 | 信号 / 槽 | 说明 |
|------|-----------|------|
| UI→业务 | `SBOControlWidget::submitSelect(SboSelectRequest, user)` | `ISBOManager::submitSelect()`（申请 `DeviceSboGuard` 锁） |
| 业务→UI | `ISBOManager::armedAcquired(seqId, SboDeviceKey)` | 进入 Armed，启动 5s 倒计时条 |
| 业务→UI | `ISBOManager::armedRejected(seqId, key, reason)` | 弹"设备忙/已被占用"提示（同 Key 互斥） |
| UI→业务 | `SBOControlWidget::submitOperate(seqId)` | `ISBOManager::submitOperate()` 执行 |
| 业务→UI | `ISBOManager::executingSucceeded/Failed` | 流程结束，清倒计时 |
| 业务→UI | `ISBOManager::armedCleared(reason)` | 断线/超时（30s）/取消 → 自动清 Armed + 审计 |

#### 2.7.4 交互逻辑（双重确认 + 设备级锁 UI 响应）

1. **Select**：操作员选设备+命令 → `submitSelect(req, user)`（RBAC 校验操作员/工程师/管理员）。业务层 `DeviceSboGuard::tryAcquire(SboDeviceKey{linkId,slaveId,registerAddr})` 申请设备级逻辑锁。
2. **Armed（设备级锁状态）**：成功后 `armedAcquired` → UI 进入 Armed 态，`ArmedCountdownBar` 启动 **5s 倒计时**（急停命令 **3s**）；同 `SboDeviceKey` 的并发 Select 将被 `armedRejected`（保持"同一设备仅 1 个 Armed"语义，允许 10 个 PCS 柜并行 Armed，Q-14）。
3. **Operate**：倒计时内点"Operate" → `submitOperate(seqId)` → 业务下发执行；成功后 `executingSucceeded` 清锁。
4. **安全撤销**：Armed 期间遇链路断线（`IChannel::connectionChanged(false)`）或 30s 超时 → 业务 `DeviceSboGuard::purgeTerminatedEntries()` 自动清锁并 `armedCleared` 审计，UI 倒计时条归零并置灰（FR-CTRL-07，Q-15）。**UI 绝不自行持有跨线程 QTimer 控制锁语义**——倒计时仅做视觉呈现，权威状态以 `ISBOManager` 信号为准（规避 V1.4 跨线程 QObject 生命周期问题）。
5. **权限**：急停仅管理员/工程师可触发；所有操作经 `ISBOManager` 留痕（审计日志）。

---

## 3. 核心 UI 类接口设计（Class API）

> 全部声明位于 `ui/` 命名空间（`ens::ui` STATIC 库）。仅包含 `#include "business/..."` 与 `#include <QCustomPlot>`，**不出现任何 `IChannel` / `QSerialPort` / `QTcpSocket` 头文件**。

### 3.1 MainWindow

```cpp
// ui/MainWindow.h
#pragma once
#include <QMainWindow>
#include "ui/ThemePalette.h"
#include "business/ISBOManager.h"   // 仅接口，不依赖实现/通道

namespace ens { namespace ui {

class OverviewWidget;
class RealtimeChartWidget;
class AlarmCenterWidget;
class HistoryTrendWidget;
class ConfigWidget;
class DiagWidget;
class SBOControlWidget;
class DrillDownNavigator;
class GlobalStatusBar;
class CommStatusBar;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(ISBOManager* sbo,   // 依赖注入业务接口
                        QWidget* parent = nullptr);
    ~MainWindow() override;

    /// 绑定业务层（启动后调用一次；懒加载 Page 在首次进入时 bindBusiness）
    void bindBusiness(IDataAccess* data, IAlarmEngine* alarm, ISBOManager* sbo);

    /// 主题：加载暗色 QSS 并将变量宏替换为 ThemePalette 实值
    void applyTheme(const ThemePalette& pal);

protected:
    void closeEvent(QCloseEvent* e) override;   // V1.1: 持久化 geometry/state

private slots:
    void onNavigate(PageId page);                 // DrillDownNavigator 回调
    void onGlobalHealthChanged(int normal, int degraded, int offline);
    void onSboLockCountChanged(int armedCount);   // 顶栏 SBO 锁态

private:
    void setupUi();                  // 三段式主框架
    void setupStackedPages();        // 7 个 Page 入 QStackedWidget
    void setupTheme();               // AA_EnableHighDpiScaling + 字体 + DPI 变量注入
    void restoreState();             // V1.1: 从 QSettings 恢复布局/页面
    void switchLanguage(const QString& locale);  // V1.1: "zh_CN" / "en_US"

    QStackedWidget*     m_stack      = nullptr;
    DrillDownNavigator* m_navigator  = nullptr;
    GlobalStatusBar*    m_topBar     = nullptr;
    CommStatusBar*      m_bottomBar  = nullptr;
    // 7 个核心视图（懒加载：构造时 new，bindBusiness 在首次显示）
    OverviewWidget*     m_overview   = nullptr;
    RealtimeChartWidget* m_rtChart   = nullptr;
    AlarmCenterWidget*  m_alarm      = nullptr;
    HistoryTrendWidget* m_history    = nullptr;
    ConfigWidget*       m_config     = nullptr;
    DiagWidget*         m_diag       = nullptr;
    SBOControlWidget*   m_sbo        = nullptr;
    QTranslator*       m_translator = nullptr;   // V1.1: i18n 切换
};

}} // namespace ens::ui
```

### 3.2 RealtimeChartWidget

```cpp
// ui/RealtimeChartWidget.h
#pragma once
#include <QWidget>
#include <QTimer>
#include <QVector>
#include "qcustomplot.h"
#include "business/IDataAccess.h"

namespace ens { namespace ui {

/// 单通道待绘制缓冲（环形，固定上限，防 OOM）
struct ChannelBuffer {
    uint32_t pointId = 0;
    QVector<QCPGraphData> pendingSamples;   // 数据到达仅入此缓冲
    QCPGraph* graph = nullptr;
    bool visible = true;

    /// V1.4: 初始化时预分配容量，确保高频 append 不触发 realloc
    void reserveCapacity(int cap = MAX_POINTS_PER_CHANNEL) {
        pendingSamples.reserve(cap);
    }
};

class RealtimeChartWidget : public QWidget {
    Q_OBJECT
public:
    static constexpr int MAX_POINTS_PER_CHANNEL = 2000;   // 单通道上限（ADR-NFR-03）
    static constexpr int MAX_PIXELS_PER_CHANNEL = 1920;   // 像素宽度上限
    static constexpr int PENDING_WARN_THRESHOLD = 5000;   // 缓冲溢出预警

    explicit RealtimeChartWidget(QWidget* parent = nullptr);
    ~RealtimeChartWidget() override;

    /// 初始化时调用 ThemePalette::tryEnableOpenGL(m_plot) 探测 GPU 可用性
    /// （V1.1：工控机无独显自动降级软件渲染，见 §4.3.2）
    ///
    /// V1.5: 构造函数中需创建 m_pauseBanner（QLabel*），样式如下：
    ///   - 父 Widget = m_plot（悬浮于图表视口之上）
    ///   - QSS：半透明深色背景 + 圆角 + 右上角绝对定位（margin-right/top 各 8dp）
    ///   - 默认 hidden；setRenderMode(Paused/Panning) 时 show()，Live 时 hide()
    ///   - 点击 Banner → emit 自定义信号或直接调用 setRenderMode(Live) 恢复实时
    ///   示例 QSS 片段：
    ///     QLabel#PauseBanner {
    ///       background: rgba(26, 26, 46, 0.85);
    ///       color: #00d4ff;
    ///       border-radius: 4px;
    ///       padding: 4px 10px;
    ///       font-size: var(--dpi-12);
    ///     }

    /// 订阅实时数据（业务层 IDataAccess 信号 → 本 Widget 槽）
    /// V1.4: 注册通道时必须对每个 ChannelBuffer 调用 buf.reserveCapacity()，
    ///       确保 std::swap 后高频 append 不触发 realloc（见 §4.2 swap 注释）。
    void bindBusiness(IDataAccess* data);

    enum class RefreshRate { Hz30, Hz60 };
    void setRefreshRate(RefreshRate r);   // 切换 30/60Hz 批处理

    /// V1.2 渲染交互态：Live 实时滚动 / Paused 暂停自动滚动 / Panning 手动平移
    enum class RenderMode { Live, Paused, Panning };
    void setRenderMode(RenderMode mode);  // Pause / 恢复实时（回放后台缓冲）

public slots:
    /// 数据到达入口：仅入队，严禁 replot()（NFR-PERF-13）
    void onRealtimeSample(uint32_t pointId, double t, double v);
    /// V1.2 通道显隐切换：隐藏时清空滞留缓冲，避免恢复可见瞬间回灌旧数据
    void onChannelToggled(uint32_t pointId, bool visible);

private slots:
    /// 唯一重绘入口：QTimer 30/60Hz 触发
    void onBatchRepaint();

protected:
    /// V1.2 鼠标按下进入交互态（暂停自动滚动），松手恢复实时
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    /// V1.2 页面显示时刷新 GL 上下文，规避 QStackedWidget 隐藏/显示黑屏
    void showEvent(QShowEvent* e) override;
    /// V1.4 后台隐藏时挂起 m_repaintTimer，避免不可见 Widget 空转 CPU
    void hideEvent(QHideEvent* e) override;
    /// V1.3 i18n：QCustomPlot 轴标签不继承 QWidget，需手动响应 LanguageChange
    void changeEvent(QEvent* e) override;

private:
    QCustomPlot*    m_plot = nullptr;
    QTimer*         m_repaintTimer = nullptr;
    QVector<ChannelBuffer> m_channels;
    bool            m_anyPending = false;

    // ══ V1.2 交互态竞用保护成员 ══
    RenderMode      m_mode = RenderMode::Live;   // 默认实时滚动
    bool            m_userInteracting = false;    // 拖拽/平移中：暂停重绘与视图重置
    QVector<QCPGraphData> m_backlog;              // 交互态后台全分辨率缓冲（限长）

    // ══ V1.5 交互态显性化：Paused/Panning 时右上角半透明悬浮 Banner ══
    QLabel*         m_pauseBanner = nullptr;      // "⏸ 实时滚动已暂停 — [点击恢复实时]"
};

}} // namespace ens::ui
```

### 3.3 SBOControlWidget

```cpp
// ui/SBOControlWidget.h
#pragma once
#include <QWidget>
#include <QProgressBar>
#include "business/ISBOManager.h"
#include "business/SboTypes.h"   // SboDeviceKey / SboSelectRequest / SboCommand

namespace ens { namespace ui {

class SBOControlWidget : public QWidget {
    Q_OBJECT
public:
    explicit SBOControlWidget(ISBOManager* sbo, QWidget* parent = nullptr);
    ~SBOControlWidget() override;

    void bindBusiness(ISBOManager* sbo);

private slots:
    // —— UI → 业务 下发 ——
    void onSelectClicked();
    void onOperateClicked();
    void onCancelClicked();
    void onEmergencyStopClicked();

    // —— 业务 → UI 状态呈现（权威状态以信号为准）——
    void onArmedAcquired(const QString& seqId, const SboDeviceKey& key);
    void onArmedRejected(const QString& seqId, const SboDeviceKey& key, const QString& reason);
    void onExecutingSucceeded(const QString& seqId, const QString& device);
    void onExecutingFailed(const QString& seqId, const QString& device, const QString& reason);
    void onArmedCleared(const QString& reason);   // 断线/超时/取消

    void onCountdownTick();   // 仅更新 ArmedCountdownBar 视觉

private:
    ISBOManager*   m_sbo = nullptr;
    QProgressBar*  m_countdown = nullptr;
    QTimer*        m_countdownTimer = nullptr;   // 仅视觉倒计时，非锁权威
    QString        m_currentSeqId;
    int            m_remainingMs = 0;            // 5s 常规 / 3s 急停
};

}} // namespace ens::ui
```

### 3.4 AlarmCenterWidget

```cpp
// ui/AlarmCenterWidget.h
#pragma once
#include <QWidget>
#include <QTableView>
#include "business/IAlarmEngine.h"
#include "business/AlarmTypes.h"   // AlarmRecord / AlarmLevel / AlarmStatus

namespace ens { namespace ui {

class AlarmTableModel;
class AlarmItemDelegate;
class AlarmPopup;

class AlarmCenterWidget : public QWidget {
    Q_OBJECT
public:
    explicit AlarmCenterWidget(IAlarmEngine* engine, QWidget* parent = nullptr);
    ~AlarmCenterWidget() override;

    void bindBusiness(IAlarmEngine* engine);

private slots:
    // —— 业务 → UI ——
    void onAlarmTriggered(const AlarmRecord& a);     // Critical → 声光弹窗
    void onAlarmRecovered(uint64_t id);
    void onAlarmAcknowledged(uint64_t id);
    void onBlackBoxRequested(uint32_t pointId, uint64_t alarmTime);

    // —— UI → 业务（V1.1: 按钮文本统一 tr() 包裹，支持 i18n，见 §4.5）——
    void onAcknowledgeSelected();    // tr("确认选中")
    void onAcknowledgeAll();         // tr("全部确认")
    void onClearRecovered();         // tr("清除恢复")
    void onReplayRequested(uint64_t alarmId);  // tr("黑匣子回放")

private:
    IAlarmEngine*    m_engine = nullptr;
    QTableView*      m_table  = nullptr;
    AlarmTableModel* m_model  = nullptr;
    AlarmItemDelegate* m_delegate = nullptr;
    AlarmPopup*      m_popup  = nullptr;
};

}} // namespace ens::ui
```

### 3.5 i18n 枚举转换工具类 QStringUtils（V1.5）

```cpp
// ui/QStringUtils.h
#pragma once
#include <QString>
#include <QObject>

// 前向声明业务层枚举（避免 UI 头文件依赖完整业务定义）
namespace ens::business {
enum class AlarmLevel : uint8_t { Info, Warning, Critical };
enum class SlaveHealth : uint8_t { Healthy, Degraded, Isolated, Offline };
enum class SboState : uint8_t { Idle, Selected, Armed, Executing, Timeout, Failed };
enum class LinkStatus : uint8_t { Online, Degraded, Offline };
}

namespace ens::ui {

/// @brief 工业上位机 i18n 枚举→可读字符串转换工具
///
/// 设计动机（V1.5 Code Review R5）：
///   工业上位机界面中存在大量基于枚举的动态文本，例如：
///     - AlarmLevel::Critical → "严重告警" / "Critical"
///     - SlaveHealth::HEALTHY  → "正常"      / "Normal"
///   这些文本需随语言动态切换（QTranslator::switchLanguage()）。
///   若直接在 Delegate::paint() 或 setData() 中硬编码中文/英文，
///   语言切换后已渲染的列表项/卡片不会自动更新。
///
/// 解决方案：
///   将所有枚举→字符串映射集中到本工具类，每个函数内部统一使用 tr() 包裹。
///   语言切换触发 changeEvent(LanguageChange) → 调用 model->update()/view->reset()
///   → Delegate 重新调用 formatXxx() → 拿到新语言文本 → 自动刷新显示。
///
/// 使用方式：
///   delegate->paint() 中：painter->drawText(rect, QStringUtils::formatAlarmLevel(level));
///   model->data() 中：    return QStringUtils::formatSlaveHealth(health);
class QStringUtils : public QObject {
    Q_OBJECT
public:
    /// 告警级别 → 本地化文本
    static QString formatAlarmLevel(ens::business::AlarmLevel level) {
        switch (level) {
        case ens::business::AlarmLevel::Info:    return QObject::tr("提示");
        case ens::business::AlarmLevel::Warning: return QObject::tr("一般告警");
        case ens::business::AlarmLevel::Critical: return QObject::tr("严重告警");
        default:                                  return QObject::tr("未知");
        }
    }

    /// 从站健康状态 → 本地化文本
    static QString formatSlaveHealth(ens::business::SlaveHealth health) {
        switch (health) {
        case ens::business::SlaveHealth::Healthy:   return QObject::tr("正常");
        case ens::business::SlaveHealth::Degraded:  return QObject::tr("降级");
        case ens::business::SlaveHealth::Isolated:  return QObject::tr("隔离");
        case ens::business::SlaveHealth::Offline:   return QObject::tr("离线");
        default:                                    return QObject::tr("未知");
        }
    }

    /// SBO 控制状态 → 本地化文本（用于 SBOControlWidget 状态栏）
    static QString formatSboState(ens::business::SboState state) {
        switch (state) {
        case ens::business::SboState::Idle:       return QObject::tr("空闲");
        case ens::business::SboState::Selected:   return QObject::tr("已选中");
        case ens::business::SboState::Armed:      return QObject::tr("就绪（ Armed ）");
        case ens::business::SboState::Executing:  return QObject::tr("执行中");
        case ens::business::SboState::Timeout:    return QObject::tr("超时");
        case ens::business::SboState::Failed:     return QObject::tr("失败");
        default:                                   return QObject::tr("未知");
        }
    }

    /// 通信链路状态 → 本地化文本（用于 DiagWidget LED 旁标签）
    static QString formatLinkStatus(ens::business::LinkStatus status) {
        switch (status) {
        case ens::business::LinkStatus::Online:    return QObject::tr("在线");
        case ens::business::LinkStatus::Degraded:  return QObject::tr("降级");
        case ens::business::LinkStatus::Offline:   return QObject::tr("离线");
        default:                                    return QObject::tr("未知");
        }
    }

    /// 通用布尔状态（用于开关类控件显示文本）
    static QString formatEnabled(bool enabled) {
        return enabled ? QObject::tr("启用") : QObject::tr("禁用");
    }
};

} // namespace ens::ui
```

**集成要点**：

| 集成位置 | 调用方式 | 语言切换响应 |
|---------|---------|------------|
| `AlarmItemDelegate::paint()` | `QStringUtils::formatAlarmLevel(alarm.level)` | `changeEvent` → `m_table->update()` |
| `AlarmTableModel::data()` | `QStringUtils::formatAlarmLevel(...)` | `changeEvent` → `beginResetModel()` |
| `SBOControlWidget` 状态栏 | `QStringUtils::formatSboState(sbo.state)` | `changeEvent` → `ui->label->setText(...)` |
| `DiagWidget` 链路表 | `QStringUtils::formatLinkStatus(link.status)` | `changeEvent` → `model->refresh()` |

> **注意**：本类为纯静态工具（无成员变量、无虚函数），编译器通常内联优化。`QObject::tr()` 需要 Q_OBJECT 宏上下文，因此继承 `QObject` 以确保翻译系统正确提取 `.ts` 条目。实际使用中也可改为**自由函数 + 手动 `qtTrId()`** 方式（不依赖 QObject 继承），按项目编码规范选择。

---

## 4. UI 性能优化与渲染降采样设计

### 4.1 数据通路：ens::datahub → UI 渲染线程

```
采集/解析线程 (Worker)
   │  Sample → L1 RingBuffer (alignas(16), 无锁 acquire/release)   [datahub]
   │
   ├─→ AlarmEngine 线程 (HIGH) 订阅 → alarmTriggered(Queued) ──→ AlarmCenterWidget
   │
   └─→ UI 主线程（仅消费者）
          │  IDataAccess::realtimeSampleReady(pointId, sample)
          │      ↓  仅写入 ChannelBuffer.pendingSamples（不重绘）
          │
          ▼  QTimer 30/60Hz (m_repaintTimer)
       onBatchRepaint()
          │  ① 提取 pendingSamples（acquire 读，无锁）
          │  ② 点数 > 2000 或像素 > 1920 → DownSampler::adaptiveDownsample()
          │  ③ QCPGraph::setData(downsampled)   （复用 QVector，避免每帧 new）
          │  ④ m_plot->replot(QCustomPlot::rpQueuedReplot)   // 同帧合并
```

**关键约束（NFR-PERF-13 / ADR-NFR-03）**：
- UI 线程是 L1 RingBuffer 的**消费者之一**，仅 `acquire` 读，绝不写；
- 数据到达路径**严禁**直接 `replot()`，必须经 `QTimer` 批处理；
- 所有跨线程数据投递使用 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`，工作线程**不触碰任何 `QWidget` / `QCustomPlot` 子对象**；
- 降采样在 UI 线程内完成（CPU 占用低，见 4.3），避免占用采集/解析热路径。

### 4.2 QTimer 批量重绘与单通道 ≤2000 点降采样伪代码

```cpp
// ui/RealtimeChartWidget.cpp —— V1.5 渲染降采样约束（源自 NFR §2.3）
void RealtimeChartWidget::onRealtimeSample(uint32_t pointId, double t, double v) {
    // ① 数据到达：只入队，绝不 replot()
    auto* buf = findChannel(pointId);
    if (!buf) return;

    // ══ V1.2 隐藏通道数据溢出防护 ══
    // 用户隐藏某通道（buf->visible=false）后，若仍持续入队 pendingSamples，
    // PENDING_WARN_THRESHOLD=5000 的硬上限虽能防 OOM，但长期隐藏仍会造成
    // 无意义的内存堆积（例如隐藏 8 小时 × 100ms 采集 = 28.8 万点滞留）。
    // 故隐藏通道直接丢弃实时样本（实时曲线本就是"当前窗口"语义，无需为
    // 隐藏通道保留历史）；待用户重新勾选可见时，从当前时刻起重新滚动绘制。
    if (!buf->visible) return;

    buf->pendingSamples.append(QCPGraphData{t, v});
    // 缓冲硬上限保护（防 OOM，对应 m_pendingStorm 思路）
    if (buf->pendingSamples.size() > PENDING_WARN_THRESHOLD) {
        buf->pendingSamples.remove(0, buf->pendingSamples.size() - MAX_POINTS_PER_CHANNEL);
    }
    m_anyPending = true;
}

// ══ V1.2 通道显隐切换：隐藏时立即清空滞留缓冲，避免恢复可见瞬间回灌旧数据 ══
void RealtimeChartWidget::onChannelToggled(uint32_t pointId, bool visible) {
    auto* buf = findChannel(pointId);
    if (!buf) return;
    buf->visible = visible;
    if (!visible) {
        buf->pendingSamples.clear();   // 释放滞留内存；下次可见时从当前时刻滚动
    }
    m_plot->replot(QCustomPlot::rpQueuedReplot);  // 即时反映显隐（唯一允许的非批处理重绘点）
}

void RealtimeChartWidget::onBatchRepaint() {
    Q_ASSERT_X(QThread::currentThread() == qApp->thread(),
               "onBatchRepaint", "must run on UI thread");
    if (!m_anyPending) return;

    bool anyUpdate = false;
    const int pixelW = m_plot->viewport().width();   // 实际像素宽（≤1920 约束）
    const int targetPoints = (pixelW > 0 && pixelW < MAX_PIXELS_PER_CHANNEL)
                                 ? pixelW : MAX_POINTS_PER_CHANNEL;

    for (auto& buf : m_channels) {
        if (!buf.visible || buf.pendingSamples.isEmpty()) continue;

        // ══ V1.1: 零拷贝转移 —— std::swap O(1) 转移内部指针，无内存分配/数据拷贝 ══
        // ══ V1.4: 容量保留保证 ══
        //   swap 后 buf.pendingSamples 变为空 QVector（size=0, capacity=原 capacity）。
        //   若初始化时已调用 reserveCapacity(MAX_POINTS_PER_CHANNEL=2000)，则：
        //     - swap 交过来的 ready 拥有 ≥2000 的 capacity
        //     - buf.pendingSamples 保留 ≥2000 的 capacity（来自上一次 swap 回来的 ready）
        //   因此后续高频 append() 在 capacity 范围内永不触发 realloc（O(1) 均摊）。
        //   关键前提：bindBusiness() 注册通道时必须对每个 ChannelBuffer 调用
        //             buf.reserveCapacity()（见 struct 定义）。
        QVector<QCPGraphData> ready;          // ① 空构造，O(1)
        ready.swap(buf.pendingSamples);         // ② 交换内部指针/size/capacity，零拷贝

        // ══ V1.2 交互态竞用保护 ══
        // 问题：用户正在拖拽/平移 X 轴（Pause/Pan 模式）浏览历史时，每帧
        // setData() 会整体覆盖 Graph 数据并将其 X 轴范围重置为最新窗口，
        // 导致"怎么也拖不动、一松手就跳回实时"的体验问题，且历史数据丢失。
        // 方案：进入交互态（m_userInteracting=true）时不重绘、不重置视图，
        // 仅把新样本追加到后台全分辨率缓冲 m_backlog，待用户点击"恢复实时"
        // 时一次性回放，既不丢数据也不打扰浏览。
        if (m_userInteracting) {
            // 后台缓冲保留全分辨率样本（受 PENDING_WARN_THRESHOLD 限长）
            m_backlog.append(ready);
            if (m_backlog.size() > PENDING_WARN_THRESHOLD)
                m_backlog.remove(0, m_backlog.size() - PENDING_WARN_THRESHOLD);
            continue;   // 跳过 setData/replot，保持当前视图不变
        }

        // ② 降采样：点数 > 目标 或 像素 > 1920 → 自适应策略
        if (ready.size() > targetPoints) {
            // DownSampler::adaptiveDownsample：
            //   平均桶 < 200 → Min-Max（实时窗口，保留极值，几乎无损）
            //   平均桶 ≥ 200 → LTTB（历史大跨度，保留趋势，避免方波锯齿）
            ready = DownSampler::adaptiveDownsample(ready, targetPoints);
        }

        // ③ setData（true=已排序；复用 QVector 内存，避免每帧堆分配）
        buf.graph->setData(ready, true);
        anyUpdate = true;
    }

    m_anyPending = false;
    if (anyUpdate) {
        // ④ 同帧合并重绘（rpQueuedReplot 不立即 paint，等事件循环统一刷新）
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    }
}

// ══ V1.2 交互态切换 ══
void RealtimeChartWidget::setRenderMode(RenderMode mode) {
    m_mode = mode;
    m_userInteracting = (mode != RenderMode::Live);

    // ══ V1.5: 交互态显性化 Banner —— Paused/Panning 时悬浮提示，防止操作员误判"数据卡死/通信中断" ══
    if (m_pauseBanner) {
        if (mode == RenderMode::Live) {
            m_pauseBanner->hide();                  // 恢复实时：隐藏 Banner
        } else {
            QString text = (mode == RenderMode::Paused)
                ? tr("⏸ 实时滚动已暂停 — [点击恢复实时]")
                : tr("✋ 平移浏览中 — [点击恢复实时]");
            m_pauseBanner->setText(text);
            m_pauseBanner->show();                  // 悬浮于图表右上角（QSS 定位）
        }
    }

    if (mode == RenderMode::Live) {
        // ══ V1.3: m_backlog 恢复实时前的硬限清理策略 ══
        // 场景：操作员在 Pause/Pan 模式下离开工位（m_userInteracting=true）持续数小时，
        //       m_backlog 虽受 PENDING_WARN_THRESHOLD=5000 限长，但点击"恢复实时"时
        //       对数千点一次性执行 DownSampler + setData 仍可能造成渲染帧间界面微卡。
        // 策略：先判断 backlog 规模，>2000 点走降采样回放路径（保留趋势，几乎无损），
        //       或提供"跳过历史直接恢复当前"的轻量复位选项。
        const int BACKLOG_REPLAY_LIMIT = 2000;   // 回放阈值：超过此值强制降采样

        if (!m_backlog.isEmpty()) {
            bool needsDownsample = (m_backlog.size() > BACKLOG_REPLAY_LIMIT);

            for (auto& buf : m_channels) {
                if (!buf.visible || m_backlog.isEmpty()) continue;

                QVector<QCPGraphData> replay = m_backlog;

                if (needsDownsample) {
                    // 大量积压：先降采样到目标点数再回放，避免单帧 O(N) setData 卡顿
                    replay = DownSampler::adaptiveDownsample(replay, MAX_POINTS_PER_CHANNEL);
                    // 可选：若用户更在意"立即看到最新"而非"补全中间过程"，
                    //       可在此处仅取末尾 MAX_POINTS_PER_CHANNEL 个样本直接赋值：
                    //   replay = replay.mid(replay.size() - MAX_POINTS_PER_CHANNEL);
                    //       这等价于"跳过历史、恢复当前"的轻量路径。
                } else if (replay.size() > MAX_POINTS_PER_CHANNEL) {
                    replay = DownSampler::adaptiveDownsample(replay, MAX_POINTS_PER_CHANNEL);
                }

                buf.graph->setData(replay, true);
            }
            m_backlog.clear();
            // 自动滚动 X 轴到最新（取数据末尾时间窗）
            m_plot->xAxis->setRange(QCPRange());  // 由后续 onBatchRepaint 自动更新
        }
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    }
}

// 鼠标按下即进入"交互态"（拖拽/平移），松开若非 Pan 模式则恢复实时
void RealtimeChartWidget::mousePressEvent(QMouseEvent* e) {
    if (m_plot->axisRect()->rect().contains(e->pos())) {
        m_userInteracting = true;   // 暂停自动滚动，允许用户浏览
    }
    QWidget::mousePressEvent(e);
}
void RealtimeChartWidget::mouseReleaseEvent(QMouseEvent* e) {
    // 仅当显式处于 Panning(手动平移) 模式才保持交互；否则视为临时拖选，恢复实时
    if (m_mode == RenderMode::Live) {
        setRenderMode(RenderMode::Live);  // 复用：回放后台缓冲 + 复位
    }
    QWidget::mouseReleaseEvent(e);
}
```

**`DownSampler` 接口（引用 NFR V1.2 / datahub）**：

```cpp
// datahub/DownSampler.h（业务层提供，UI 直接调用）
class DownSampler {
public:
    enum class Strategy { MinMax, LTTB };
    static QVector<QCPGraphData> adaptiveDownsample(
        const QVector<QCPGraphData>& input, int targetCount) {
        const size_t avgBucket = input.size() / static_cast<size_t>(targetCount);
        return (avgBucket > 200) ? lttb(input, targetCount)
                                 : minMaxBucket(input, targetCount);
    }
    static QVector<QCPGraphData> minMaxBucket(const QVector<QCPGraphData>&, int); // 实时窗口
    static QVector<QCPGraphData> lttb(const QVector<QCPGraphData>&, int);          // 历史大跨度
};
```

### 4.3 工程约束与验收指标

| 维度 | 约束 | 验收指标（引用 NFR） |
|------|------|---------------------|
| 渲染帧率 | QTimer 30Hz（默认）/ 60Hz（高性能站）+ Min-Max ≤2000 点 + `rpQueuedReplot` | **≥ 30 FPS / ≥ 58 FPS**（Q-06，PERF-T-04） |
| CPU 占用 | 严禁数据驱动 `replot()`；无锁热路径 + 线程隔离 | **< 15%**（Q-01，对比基线关降采样 CPU > 45%） |
| 缓冲保护 | `pendingSamples` 硬上限 `PENDING_WARN_THRESHOLD=5000`，溢出滚动丢弃 | 缓冲区不溢出（PERF-T-04 附加检查） |
| 内存 | `setData` 复用 `QVector`（`std::swap` 零拷贝），无每帧堆分配；OpenGL 可选加速 | 72h 增长 < 5%（Q-03） |
| 响应延迟 | 三级钻取 `< 200ms`；冷启动 `< 5s` 至总览可用 | Q-09/Q-10 |
| 模块隔离 | `ens::ui` STATIC，仅依赖 `ens::business` + `qcustomplot`；禁止 `IChannel` 直调 | CMake 编译期守卫（`target_link_libraries(ens_ui PUBLIC ens::business qcustomplot)`，**不链** `ens::channel`） |
| 跨线程 | 所有业务→UI 经 `Qt::QueuedConnection`；UI 不持有工作线程 `QTimer`/锁 | 无跨线程 QWidget 访问（规避 V1.4 跨线程 QObject 问题） |

#### 4.3.1 CMake 构建约束（ens::ui STATIC）

```cmake
# ui/CMakeLists.txt
add_library(ens_ui STATIC)
target_sources(ens_ui PRIVATE
    MainWindow.cpp RealtimeChartWidget.cpp AlarmCenterWidget.cpp
    HistoryTrendWidget.cpp ConfigWidget.cpp DiagWidget.cpp SBOControlWidget.cpp
    OverviewWidget.cpp ThemePalette.cpp DrillDownNavigator.cpp)
target_include_directories(ens_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ens_ui PUBLIC
    ens::business      # 仅业务接口层
    qcustomplot)       # 图表库
# 注意：严禁 target_link_libraries(ens_ui PUBLIC ens::channel)
target_compile_features(ens_ui PUBLIC cxx_std_17)
```

#### 4.3.2 OpenGL 加速兼容性降级预案（工控机无独显场景）

**问题**：设计默认启用 `m_plot->setOpenGl(true)` 以获得硬件加速。但实际工业上位机常运行于：
- 无独立显卡的工控机（仅集成 Intel HD Graphics / AMD APU）；
- 嵌入式 Linux / 国产 OS（如银河麒麟、统信 UOS），OpenGL 驱动成熟度参差；
- 部分虚拟化/远程桌面环境，GPU 直通不可用。

上述场景下 OpenGL 驱动可能存在兼容性问题，导致 **渲染黑屏、显存泄漏或进程崩溃**。

**方案：启动时探测 + 自动降级**

```cpp
// ui/ThemePalette.cpp —— V1.1 OpenGL 探测与降级
bool ThemePalette::tryEnableOpenGL(QCustomPlot* plot) {
    // 从配置文件读取用户偏好（默认 true）
    const bool userWantsGl = m_config.value("ui/enable_opengl", true).toBool();
    if (!userWantsGl) {
        plot->setOpenGl(false);
        return false;
    }

    // 尝试启用 OpenGL 并验证上下文可用性
    plot->setOpenGl(true);

    // 强制触发一次空 replot 以检测 GL 上下文是否真���创建成功
    // （部分驱动 setOpenGl(true) 不报错但实际渲染时崩溃）
    try {
        plot->replot(QCustomPlot::rpQueuedReplot);
        // 若到达此处且无异常/GL 错误 → 硬件加速生效
        return true;
    } catch (...) {
        // 捕获任何异常（某些 Qt 平台 GL 初始化抛异常而非返回错误码）
    }

    // 回退路径：禁用 OpenGL，改用纯软件光栅化（Software Rendering）
    qWarning() << "[UI] OpenGL initialization failed, falling back to Software Rendering";
    plot->setOpenGl(false);
    plot->replot(QCustomPlot::rpQueuedReplot);   // 软件渲染验证
    return false;
}
```

| 场景 | 行为 | 日志 |
|------|------|------|
| 有 GPU + 驱动正常 | `setOpenGl(true)` 成功 | `[UI] OpenGL enabled` |
| 用户配置关闭 | 直接软件渲染 | `[UI] OpenGL disabled by config` |
| 无 GPU / 驱动异常 | 自动回退 `setOpenGl(false)` | `[UI] OpenGL init failed, fallback to Software` |
| 虚拟化环境 | 同上 + 标记 `m_glAvailable=false` 影响帧率策略 | 帧率自动降至 30Hz |

> **工程约束**：`RealtimeChartWidget` 构造时调用 `ThemePalette::tryEnableOpenGL(m_plot)` 而非直接 `setOpenGl(true)`；若检测到降级，`setRefreshRate()` 默认选 `Hz30`（软件渲染下 60Hz CPU 开销过高）。

#### 4.3.3 OpenGL Widget 在 QStackedWidget 隐藏/显示时的上下文丢失修复

**问题（V1.2 新增）**：实时曲线页（`RealtimeChartWidget` 内含 GL 加速的 `QCustomPlot`）作为 `QStackedWidget` 的一个 Page 存在。当用户切走（页面被 `QStackedWidget::setCurrentIndex` 隐藏）再切回时：
- 在部分 X11 / Wayland / Windows 驱动下，被隐藏的 GL 窗口会丢失 OpenGL 上下文（Context），重新显示时画面**黑屏或短暂闪烁**；
- Qt 的 `QOpenGLWidget` 在 `hideEvent` 后会释放并重建上下文，若重建时机晚于首帧 `replot`，会出现"回到曲线页先黑一下再出图"的现象。

**方案：页面 `showEvent` 时强制刷新 GL 缓冲**

在 `RealtimeChartWidget` 重写 `showEvent()`，页面由非可见变为可见时主动触发一次视口刷新，确保重建后的 GL 上下文立即重绘最新数据：

```cpp
// ui/RealtimeChartWidget.cpp —— V1.2 页面显示时刷新 GL 上下文（V1.4 增加定时器智能挂起）
void RealtimeChartWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    // ══ V1.4: 智能恢复定时器 ══
    // 页面重新可见时重启 30/60Hz 批处理重绘定时器。
    // 即使页面不可见期间 onBatchRepaint 仍可能被事件循环触发（若 timer 未停），
    // 但此时 replot 对不可见 Widget 无意义，白白消耗 CPU 轮询开销。
    if (m_repaintTimer && !m_repaintTimer->isActive()) {
        m_repaintTimer->start();
    }

    // 切回本页：强制视口更新 + 重绘，规避隐藏期间 GL 上下文释放导致的黑屏/闪烁
    // rpQueuedReplot 经事件循环统一刷新，不阻塞 showEvent
    if (m_plot) {
        m_plot->viewport()->update();                 // 触发 GL 缓冲刷新
        m_plot->replot(QCustomPlot::rpQueuedReplot);  // 重绘最新一帧
    }
    // 若离开期间累积了 pendingSamples，立即补一帧（避免切回瞬间空白）
    if (m_anyPending) onBatchRepaint();
}

// ══ V1.4: 后台隐藏时挂起定时器，避免空转 CPU ══
// 当 RealtimeChartWidget 被 QStackedWidget 切换至后台隐藏时，
// 30Hz/60Hz 的 QTimer 仍会持续触发 onBatchRepaint() → setData() → replot()，
// 但此时 Widget 不可见，所有渲染输出均被丢弃，纯属浪费 CPU 轮询。
void RealtimeChartWidget::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    if (m_repaintTimer && m_repaintTimer->isActive()) {
        m_repaintTimer->stop();   // 挂起：停止无用轮询
        // 注意：不清理 pendingSamples / backlog——数据继续入队，待 showEvent 恢复后补帧
    }
}

// ══ V1.3 i18n：QCustomPlot 轴标签不继承 QWidget，不响应 changeEvent(LanguageChange) ══
// §4.5.2 已说明 tr() 包裹用户可见字符串，但 QCPAxis 的 setLabel() 设置的文本
// 存储在 QCPAxis 内部 QString 成员中，不受 Qt 翻译系统自动刷新。
// 因此必须在 Widget 层手动拦截 LanguageChange 事件，重新设置轴标签。
void RealtimeChartWidget::changeEvent(QEvent* e) {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::LanguageChange && m_plot) {
        // 重新设置所有轴标签（tr() 在新语言下返回翻译后文本）
        m_plot->xAxis->setLabel(tr("时间 (s)"));
        m_plot->yAxis->setLabel(tr("测量值"));
        // legend 文本由 QCP 自身管理，无需额外处理
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    }
}
```

| 场景 | 行为 | 结果 |
|------|------|------|
| 切走再切回（GL 上下文重建） | `showEvent` 触发 `viewport()->update()` + `replot()` | 立即出图，无黑屏闪烁 |
| 软件渲染降级（`setOpenGl(false)`） | `showEvent` 同样刷新，无 GL 上下文问题 | 正常 |
| 离开期间有数据累积 | `showEvent` 末调用 `onBatchRepaint()` 补帧 | 切回即见最新窗口，无空白 |

> **注意**：刷新必须走 `rpQueuedReplot`（非立即 `rpImmediateReplot`），否则在 `showEvent` 期间强行同步 paint 可能再次触发上下文竞争。

### 4.4 High DPI 自适应与 QSS 变量集中化

#### 4.4.1 问题：QSS px 硬编码在缩放下变形

§1.2.3 的 QSS 示例中出现了硬编码像素值（如 `border-radius: 6px; padding: 6px 14px; width: 10px`）。尽管已开启 `Qt::AA_EnableHighDpiScaling`，在 **1080P (100%) → 4K (200%/300%) 缩放**切换时：
- 部分 Qt 版本/平台对 QSS 中 `px` 单位的 DPI 缩放行为不一致（Qt 5.15 部分平台不缩放 QSS px）；
- 复杂控件（如嵌套 `QSplitter` + `QTableView` + 自定义 `ItemDelegate`）的固定像素间距可能导致布局挤压、文字重叠。

#### 4.4.2 方案：QSS 变量集中定义 + DPI 动态注入

**① 将基础尺寸抽象为 QSS 变量（支持运行时替换）：**

```css
/* theme/ens_dark.qss（V1.1，全部尺寸通过变量引用） */
:root {
    /* ---- 色彩变量（不变）---- */
    --bg-base: #1a1a2e; --bg-panel: #16213e; --accent: #00d4ff;
    --alarm-critical: #ff3b3b; --alarm-warning: #ffcc00; --alarm-info: #4aa3ff;
    --status-normal: #3ad29f;

    /* ---- 尺寸变量（V1.1 新增，由 ThemePalette 根据 DPI 注入实值）---- */
    --radius-sm: var(--dpi-4);        /* 小圆角：4dp × scale */
    --radius-md: var(--dpi-6);        /* 中圆角：6dp × scale */
    --radius-lg: var(--dpi-8);        /* 大圆角：8dp × scale */
    --pad-xs:   var(--dpi-4);         /* 内边距 XS */
    --pad-sm:   var(--dpi-6);         /* 内边距 SM */
    --pad-md:   var(--dpi-10) var(--dpi-14);  /* 内边距 MD (上下 左右) */
    --border-w: 1px;                  /* 描边宽度（1px 在所有 DPI 下保持物理 1px） */
    --scrollbar-w: var(--dpi-10);     /* 滚动条宽 */
}

/* 使用示例（不再出现裸数字） */
QWidget#Panel {
    background: var(--bg-panel);
    border: var(--border-w) solid var(--border);
    border-radius: var(--radius-md);
}
QPushButton#Primary {
    background: var(--accent);
    color: #06121f;
    border-radius: var(--radius-sm);
    padding: var(--pad-md);
}
```

**② `ThemePalette` 启动时根据 `screen()->logicalDotsPerInch()` 计算并注入 `--dpi-N` 变量：**

```cpp
// ui/ThemePalette.cpp —— V1.1 DPI 感知注入
void ThemePalette::injectDpiVariables(QString& qss) {
    const qreal dpiScale = qApp->primaryScreen()->logicalDotsPerInch() / 96.0;
    // 四舍五入到整数 px（避免亚像素渲染模糊）
    auto dp = [dpiScale](int baseDp) -> int {
        return qRound(baseDp * dpiScale);
    };
    qss.replace("--dpi-4",  QString::number(dp(4)))
       .replace("--dpi-6",  QString::number(dp(6)))
       .replace("--dpi-8",  QString::number(dp(8)))
       .replace("--dpi-10", QString::number(dp(10)))
       .replace("--dpi-14", QString::number(dp(14)));
}
```

**③ 关键界面强制自适应比例布局：**

对于拓扑图等矢量界面（`OverviewWidget::TopoView`），除 QSS 变量外还需额外保障：

```cpp
// ui/OverviewWidget.cpp —— V1.1 fitView + Transform 缩放
void OverviewWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    // fitInView 使拓扑图自适应容器大小，保持长宽比不拉伸变形
    if (m_topoView && m_topoView->scene()) {
        m_topoView->fitInView(m_topoView->scene()->sceneRect(), Qt::KeepAspectRatio);
        // 可选：对极端宽屏（如 21:9 超宽屏）额外限制最大缩放倍数
        const qreal maxScale = 2.0;
        if (m_topoView->transform().m11() > maxScale) {
            m_topoView->scale(maxScale / m_topoView->transform().m11(),
                               maxScale / m_topoView->transform().m22());
        }
    }
}
```

**④ V1.2 修复：拓扑连线在 High DPI 缩放下的线宽抖动**

`fitInView` / `scale()` 会改变 `QGraphicsView` 的视图变换矩阵，使得普通 `QPen` 在 4K（200%+ 缩放）下被同步放大——连线要么过粗、要么在缩放回 1:1 时过细，呈现"线宽随缩放跳变"的观感。

**方案**：所有拓扑矢量边（通信链路 `LinkEdge`、节点描边）使用 **Cosmetic Pen**（`QPen::setCosmetic(true)`），使其在任意视图缩放下始终保持**固定物理像素宽度**（与变换矩阵无关）：

```cpp
// ui/OverviewWidget.cpp —— V1.2 通信链路连线使用 Cosmetic Pen
QPen makeLinkPen(const QColor& c, qreal physPx = 1.5) {
    QPen pen(c, physPx);
    pen.setCosmetic(true);   // 关键：忽略视图缩放，固定 1.5 物理像素宽
    pen.setCapStyle(Qt::RoundCap);
    return pen;
}
// 创建链路连线时：
auto* edge = new QGraphicsLineItem(QLineF(p1, p2));
edge->setPen(makeLinkPen(Qt::gray));   // 不论 1080P 还是 4K，线宽恒定
// 节点选中描边同理：setCosmetic(true) 保证高亮环不随缩放变粗/变细
```

> **适用范围**：仅矢量图元（`QGraphicsLineItem` / `QGraphicsPathItem` / 自定义 `QGraphicsItem` 的 `paint()` 内 `QPen`）需要此处理；普通 `QWidget` 控件由 QSS 变量（§4.4.2）与 `AA_EnableHighDpiScaling` 统一处理，无需 Cosmetic。

### 4.5 界面状态持久化与国际化（i18n）补充

#### 4.5.1 布局状态持久化（QSettings）

工业上位机操作员习惯自定义界面布局（如诊断页 `QSplitter` 报文框与质量柱的比例、告警中心列宽、导航栏折叠状态）。应在 `MainWindow::closeEvent` 中持久化并在下次启动恢复：

```cpp
// ui/MainWindow.cpp —— V1.1 状态持久化
void MainWindow::closeEvent(QCloseEvent* e) {
    QSettings s("EnerSentry", "HMI");
    s.setValue("geometry", saveGeometry());          // 窗口位置/大小
    s.setValue("state",    saveState());              // 工具栏/Dock/Splitter 状态
    s.setValue("lastPage", m_navigator->currentPageId());  // 最后停留页面
    QMainWindow::closeEvent(e);
}

// 构造函数末尾恢复
void MainWindow::restoreState() {
    QSettings s("EnerSentry", "HMI");
    restoreGeometry(s.value("geometry").toByteArray());
    restoreState(s.value("state").toByteArray());
    PageId last = static_cast<PageId>(s.value("lastPage").toInt());
    if (last >= Page_Overview && last <= Page_SBO) {
        m_navigator->navigateTo(last);  // 恢复上次页面
    }
}
```

> **持久化范围**：仅保存 UI 几何状态；**不持久化业务数据**（如告警过滤条件、曲线通道选择），避免脏数据污染。

#### 4.5.2 国际化（i18n）预留机制

对于有出海需求或外方验收要求的储能项目，代码内所有面向用户的中文硬编码字符串应统一包裹 `tr()`，并预留 `QTranslator` 切换机制：

```cpp
// ui/AlarmCenterWidget.cpp —— V1.1 tr() 示例
// ❌ 硬编码（V1.0）：
auto* btnAck = new QPushButton("确认选中", this);

// ✅ 国际化（V1.1）：
auto* btnAck = new QPushButton(tr("确认选中"), this);
auto* btnAckAll = new QPushButton(tr("全部确认"), this);
auto* btnClear = new QPushButton(tr("清除恢复"), this);
auto* btnReplay = new QPushButton(tr("黑匣子回放"), this);
```

**`MainWindow` 中预留语言切换入口：**

```cpp
// ui/MainWindow.h —— V1.1 新增
private:
    QTranslator* m_translator = nullptr;
    void switchLanguage(const QString& locale);  // "zh_CN" / "en_US"

// ui/MainWindow.cpp
void MainWindow::switchLanguage(const QString& locale) {
    if (m_translator) { qApp->removeTranslator(m_translator); delete m_translator; }
    m_translator = new QTranslator(this);
    // 加载 :/i18n/hmi_zh_CN.qm 或 hmi_en_US.qm（lrelease 编译自 .ts 文件）
    if (m_translator->load(":/i18n/hmi_" + locale)) {
        qApp->installTranslator(m_translator);
        // 通知所有 Widget 重新翻译 UI 字符串（Qt 标准 changeEvent 流程）
        for (auto* w : findChildren<QWidget*>()) {
            w->update();   // 触发 retranslateUi 或 paintEvent 重绘
        }
    }
}
```

> **设计规范约束**：本文档后续版本中所有按钮标签、提示文本、表头均以 `tr(...)` 包裹形式书写；术语表中的中文名称为默认语言（`zh_CN`），英文对照保留在附录 C（待补充 `.ts` 文件）。

---

## 附录 A：UI 模块 ↔ SRS 需求追溯矩阵

| UI 模块 | 对应 SRS 需求 | 关键验收 |
|---------|--------------|---------|
| 电站总览 | FR-OV-01~07, UI-01/05/07 | 拓扑+指标卡，钻取 < 200ms |
| 实时曲线 | FR-RT-01~08, NFR-PERF-03/13 | 30/60Hz 批处理，≤2000 点降采样，≥30 FPS |
| 告警中心 | FR-AL-01~13, NFR-PERF-06 | Critical 声光 < 100ms，确认/清除/回放 |
| 历史趋势 | FR-HT-01~08, NFR-PERF-08 | 24h < 1s / 7d < 3s，双轴对比，导出 |
| 参数配置 | FR-CFG-01~10 | 点表/阈值热加载，校验约束 |
| 通信诊断 | FR-DG-01~06 | 多链路 LED，十六进制捕获，质量% |
| SBO 控制台 | FR-CTRL-01~07, NFR-SEC | Select-Armed-Operate，设备级锁，断线自清 |

## 附录 B：术语表

| 术语 | 含义 |
|------|------|
| SBO | Select Before Operate，预选-预置-执行 双重确认控制模式 |
| DeviceSboGuard | 设备级逻辑锁，按 `(linkId, slaveId, registerAddr)` 二维 key 分桶互斥 |
| L1 / L2 | 一级（内存环形缓冲实时数据）/ 二级（SQLite 降采样持久化）存储 |
| DrillDownNavigator | UI 三级钻取路由与防抖中枢 |
| rpQueuedReplot | QCustomPlot 重绘标志，合并同帧刷新而非立即 paint |
| Min-Max / LTTB | 两种降采样策略：极值桶（实时）/ 三角面积（历史大跨度） |
| std::swap 零拷贝 | `onBatchRepaint()` 中用 `swap()` 替代 QVector 深拷贝，O(1) 转移内部指针 |
| OpenGL 降级预案 | 启动时探测 GPU 可用性，工控机无独显自动回退 Software Rendering |
| GL 上下文丢失修复 | `QStackedWidget` 隐藏/显示时 `showEvent` 刷新 `viewport()` + `replot()`，规避黑屏/闪烁 |
| Cosmetic Pen | 拓扑矢量线 `QPen::setCosmetic(true)`，忽略视图缩放，固定物理像素线宽 |
| 隐藏通道溢出防护 | `onRealtimeSample` 对隐藏通道直接丢弃；`channelToggled(false)` 清空 pendingSamples |
| 交互态竞用保护 | `m_userInteracting` + `m_backlog`：Pause/Pan 浏览历史时不重置视图，后台保留全分辨率数据，恢复实时回放 |
| m_backlog 硬限清理 | `setRenderMode(Live)` 回放前判断 backlog 规模，>2000 点先降采样或提供"跳过历史恢复当前"轻量路径 |
| QCustomPlot i18n | 轴标签不继承 QWidget，需显式重写 `changeEvent(LanguageChange)` 手动 `setLabel(tr())` + `replot()` |
| 电芯级批量渲染 | 100+ 电芯禁止嵌套 QWidget/QLabel，改用 QGraphicsView 批量图元 或 QAbstractTableModel + 自定义 QItemDelegate 绘制 |
| Timer 智能挂起 | hideEvent 停止 m_repaintTimer（不可见 Widget 空转 CPU 浪费），showEvent 重新 start()；不清理 pendingSamples/backlog，待恢复后补帧 |
| 局部脏区刷新 | QItemDelegate 场景下 emit dataChanged(index, index, {Qt::DisplayRole}) 精准标记变更格，避免 beginResetModel() 全 View 重建 |
| Capacity 预留 | ChannelBuffer 初始化时 reserve(MAX_POINTS_PER_CHANNEL)，确保 std::swap 后高频 append 不触发 realloc |
| DPI 变量注入 | QSS 尺寸通过 `--dpi-N` 变量引用，`ThemePalette` 按 `logicalDotsPerInch()` 注入实值 |
| QSettings 持久化 | `saveGeometry()`/`saveState()` 保存窗口/Dock/Splitter 布局，下次启动恢复 |
| tr() / QTranslator | Qt 国际化机制：用户可见字符串包裹 `tr()`，运行时 `.qm` 文件切换语言 |
| PauseBanner 悬浮提示 | Paused/Panning 交互态时图表右上角半透明 Banner（`m_pauseBanner`），显性提示"实时滚动已暂停"，点击可恢复 Live 模式，防止操作员误判数据卡死 |
| DeviceCoordinateCache | QGraphicsItem 缓存模式：静态背景图元（变压器/电池轮廓）启用后，fitInView() 变换时直接复用 Pixmap，避免重复 paint() 路径光栅化 |
| QStringUtils | i18n 枚举→字符串转换工具类（formatAlarmLevel/formatSlaveHealth/formatSboState 等），内部统一 tr() 包裹，语言切换后通过 update()/resetModel() 自动刷新显示 |
