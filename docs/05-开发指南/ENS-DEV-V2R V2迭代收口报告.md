# V2 迭代收口报告 — 安全认证闭环与 SRS P0 全量清零

> 版本：v1.0（2026-09-03）｜切片 28 → 41（14 切片）｜ctest 325/325 全绿｜构建 0 警告

## 1. 范围与目标

V2 迭代承接 Phase 4 收口（P4R，切片 18-27）的遗留清单，分三阶段收官：

| 阶段 | 切片 | 目标 | 状态 |
|---|---|---|---|
| A 安全认证闭环 | 28-33 | NFR-SEC-06 哈希/锁定 + FR-AUTH-01 完整版（用户管理/首登强改密）+ 审计可视化 + UI 自动化冒烟 | ✅ |
| B SRS P0 清零 | 34-38 | 数据导出（FR-EXP-01/03）+ 告警落库/中心/声光/回放（FR-AL-06/11/12/13） | ✅ |
| C P1/P2 增强 | 39-41 | 曲线交互三件套（FR-RT-06/07/08）+ SOC 仪表盘/热力条（FR-OV-06/07）+ 导出补全（FR-EXP-02/05/06） | ✅ |

**里程碑含义**：SRS 功能需求（FR）105 条中 P0=79 条至此**代码侧全量闭环**（切片 37 声光、38 回放双收官）。V2 结束后 SRS 层面不再有未落地的 P0/P1 功能项（FR-EXP-04 裁定 P2 留档，见 §6）。

## 2. 交付清单（切片 28 → 41）

| 切片 | 主题 | 关键交付 |
|---|---|---|
| 28 | NFR-SEC-06 安全加固 | 密码 sha256$salt$hex 哈希、5 次失败锁 15 分钟、users.json 三级样例、verifyPassword 明文兼容 |
| 29 | 低告警方向 | AlarmEntities +AlarmDirection(High/Low)、AlarmRule.direction、evaluate 双向判定 + JSON 解析 |
| 30 | 审计日志可视化 | AuditLogDialog（QTableView + Admin-only 清空留痕）、工具菜单裁剪（FR-AUTH-04） |
| 31 | 用户管理 UI | AuthManager 增删改 + **随机 salt**、UserManagerDialog、save/load 往返、禁删当前登录用户 |
| 32 | 首登强改密 | mustChange 标志 + LoginDialog 强制改密流程（取消/失败不可跳过，FR-AUTH-01 闭环） |
| 33 | UI 自动化冒烟 | test_helpers 升 QApplication/offscreen、qoffscreen 插件部署、ui_smoke 3 用例（MainWindow 7 视图实例化） |
| 34 | 历史趋势 CSV | ui/common/CsvWriter（UTF-8 BOM + RFC 4180）、HistoryTrendWidget 导出按钮（FR-EXP-01） |
| 35 | 告警历史存储基座 | datahub/AlarmRecord + SQLiteDataAccess alarm 库扩展、business/AlarmRecordStore（FR-AL-13 数据面） |
| 36 | 告警中心完整版 | IAlarmAccess + queryAlarms 跨月路由、AlarmCenterWidget 筛选/确认/导出（FR-AL-11 + FR-EXP-03） |
| 37 | 严重告警声光 | ui/common/AlarmNotifier（单调时钟防抖 + 风暴抑制）、CriticalAlarmDialog 红色横幅、beep + alert（FR-AL-06） |
| 38 | 告警 ±30s 回放 | datahub/IL1SnapshotReader + AlarmReplayDialog QCustomPlot 回放（FR-AL-12，黑匣子文件级标注边界） |
| 39 | 曲线交互三件套 | RealtimePlotWidget 分轴/悬停 tracer/标尺垂线（FR-RT-06/07/08）+ 通道列表右轴分配 |
| 40 | 总览 SOC + 热力 | OverviewPoints 分类纯函数、SocGauge 240° 自绘仪表、TempHeatBar 簇温度色带（FR-OV-06/07） |
| 41 | 导出补全 | ExportUtils（目录/文件拷贝）、RealtimePlotWidget 截图 PNG、ConfigWidget 备份 tab（FR-EXP-02/05/06） |

> **git 历史说明**：切片 34-36 在实际执行中经多次 amend，最终合入单条提交 `a3f4c95`（提交消息含三切片标题）；切片 34 尚有中途版本 `d6cf31f`/`34c5575`。功能与测试与本文档逐条对应，仅提交边界不对齐，不影响回滚语义。

## 3. 架构演进（V2 新增/扩展）

```
business ── AuthManager(哈希/锁定/RBAC/审计/mustChange/用户管理) · AlarmRule.direction · AlarmRecordStore
datahub ── AlarmRecord · SQLiteDataAccess(alarm 库 DDL + IAlarmAccess + IDataAccess) · IL1SnapshotReader
ui/common ─ CsvWriter · ExportUtils · AlarmNotifier(通知决策层) · OverviewPoints(纯函数) · theme.qss
ui/controls ─ SocGauge · TempHeatBar · CriticalAlarmDialog · AlarmReplayDialog · AuditLogDialog · UserManagerDialog
ui/views ─ AlarmCenterWidget(完整版) · HistoryTrendWidget(+导出) · ConfigWidget(+备份 tab) · OverviewWidget(SOC/热力)
ui/charts ─ RealtimePlotWidget(分轴/悬停/标尺/PNG) · OpenGLDetector
cmake ─ EnsDeploy.ens_windeployqt(+offscreen 平台插件)
```

- **分层铁律延续**：UI 禁触具体类，新增查询/回放一律先立抽象（IAlarmAccess / IL1SnapshotReader）再注入 UiDeps；通知决策（AlarmNotifier）与纯算法（OverviewPoints/CsvWriter/ExportUtils/formatHoverLine）全部脱 GUI 可单测。
- **隔离语义**：告警独立库 `<dataRoot>/alarm/YYYYMM/alarm_YYYYMM.db`（DBDD §4.4），与月库 history/ 静态隔离；AlarmRecordStore 的 dataRootDir 为空 = 禁用 no-op。
- **UI 开发方式**：静态布局走 .ui 拖拽、动态内容代码实现（HJL 约定），V2 新增 9 个 .ui 均遵此例。

## 4. 测试基线

| 项 | Phase 4 收口（切片 27） | V2 收口（切片 41） | Δ |
|---|---|---|---|
| ctest | 268/268 | **325/325** | **+57 用例（13 新测试文件）** |
| 构建 | 0 错误 0 警告 | 0 错误 0 警告（MSVC /utf-8） | — |
| GUI 冒烟 | 登录窗挂起存活（人工） | ui_smoke 自动化（offscreen 7 视图实例化） | 自动化化 |
| 双进程验收 | run_phase4_acceptance 6 项 PASS | CLI 回归无破坏（切片 41 后未重跑全量脚本） | 沿用 |

新增测试文件（tests/unit/）：test_auth_security(5) · test_alarm_direction(3) · test_user_manage(6) · test_must_change(3) · test_ui_smoke(3) · test_csv_writer(5) · test_alarm_record_store(6) · test_alarm_query(7) · test_alarm_notifier(4) · test_replay(3) · test_realtime_interact(4) · test_overview_gauges(4) · test_export_utils(3)。基线演进：268(27) → 273(28) → 276(29/30) → 282(31) → 285(32) → 288(33) → 293(34) → 299(35) → 306(36) → 311(37) → 314(38) → 318(39) → 322(40) → 325(41)。

## 5. 验收证据与工具

| 证据 | 说明 |
|---|---|
| 单测全绿 | 13 测试文件 57 用例（含 6 例 UI/控件冒烟，offscreen 平台） |
| 手工验收路径 | admin/Admin@123 登录 → 审计/用户管理/告警中心筛选确认导出/曲线悬停标尺/总览仪表盘/配置备份 tab 逐页验证 |
| RBAC 行为 | operator 登录 → SBO/配置灰、恢复视图回总览、审计清空按钮禁用 |
| drill 回归 | 切片 34-41 期间未改协议/引擎热路径，overheat_fast CLI 双进程回归保持 PASS |

## 6. 已知遗留（后续阶段展望）

| 项 | 说明 | 优先级 |
|---|---|---|
| V3/后续功能方向 | SRS 仅余 FR-EXP-04（帧 hex dump，channel 无帧边界，需协议层 RxTrace 环形缓冲 ~360 行）+ TBD-01/02/03 架构预留（CAN/MySQL/OPC UA，均无硬件/平台需求） | P2 |
| simF32 特判 | EnerSentryApp.cpp 注释 TODO：sim 收口真 Float32（IEEE754 双寄存器）后删除 Uint16 解码分支 | P2 |
| modbus_engine 跨线程 flaky | 切片 11-13 期间三次偶发超时；ctest 后续连续 5+ 轮全绿未复现 | 复核后留档 |
| 黑匣子可视化 / i18n / CI | critical.swp mmap 解析展示价值低；无国际化需求；无远程仓库（tools/ci-checks 本地态） | 永久可留 |
| UI 截图/曲线交互人工项 | OpenGL 渲染、30Hz CPU<15%、悬停流畅度等留本地验收 | — |

## 7. 主要坑位沉淀（防回归，V2 实测）

1. **Q_OBJECT/头布局变更后遇 Qt5Core 内 SIGSEGV 或 getter 读到漂移垃圾值 → `ninja -t clean` 全量重编消费目标（含 tests）**（切片 36/37 两次实测：alarm-store 随机崩 Qt5Core、AlarmNotifier 赋值点正确而 getter 侧 191M 垃圾 = stale obj；38 又证 STATIC 库头加虚函数需 `clean ens_app` 单链 LNK2019）。
2. **offscreen 平台 QTimer 触发不确定**（30Hz 批渲染/500ms 刷新/1s 轮询在测试进程零次触发）→ 控件暴露 `refreshNow()`/显式 pump 收口断言。
3. **QTimer 防抖依赖事件循环 → flaky**；AlarmNotifier 改用单调时钟 steady_clock 直接比较（工程实现亦更简）。
4. **qoffscreen.dll 不在 windeployqt 默认部署集** → QApplication 构造 abort（exe 无输出 exit 1）→ EnsDeploy.cmake 必须追加 offscreen 插件拷贝。
5. **测试进程无 ens_app qrc** → `:/icons/*.svg` 空图标不崩属预期（勿当 bug）。
6. **Ui:: 前向声明必须放全局 namespace**（误放 ens::ui 内 → C2027）；QHash keys() 返回 QList，赋 QVector 成员须 `.toVector()`。
7. **CsvWriter 空列/双逗号**：全空行 `",,\r\n"` 含子串 ",\r\n"——"中列空"断言不能写 find(",\r\n")==npos。
8. **pointIdOf vs resolve**：UI 按 pointId 反查 unit/name 一律 `pointIdOf(pointId)`（resolve 是 (slave,reg) 双参，C2660 一次）；点表 loadFromJsonFile 要求 meta.schemaVersion。
9. **alarm 落库三坑**：`SQLiteDataAccess dal(QString())` 是 most vexing parse → 用 `QString{}`；insertAlarmRecords 契约"单批同月"→ 跨月 trigger 须分批，否则静默错写同月表；business 层 .cpp include 自己的头用短名（"AlarmRecordStore.h"，前缀化 C1083）。
10. **AlarmEngine 同点 Active 不重复触发** → 防抖/风暴测试第 2 次触发须换新点或先 recover；引擎恢复/确认时间戳由 store 收信号时打墙钟，重启后旧 Active 记录不可回填（一致语义，接受）。
11. **engine 风暴模式仍实时 emit Critical**（alarmTriggered）→ 弹窗/通知层需 1s 防抖聚合挡刷屏。
12. **QCustomPlot 2.1.1 API 细节**：`graph->data()` 返 QSharedPointer<QCPDataContainer>（findBegin 非 const）；tracer 样式枚举 `QCPItemTracer::tsCircle`；导出 PNG 走 `plotLayout()->itemAt(0)` qobject_cast 取 plot。
13. **QFileDialog 语义区分**：备份/配置导出选**目录**（getExistingDirectory）vs 截图/CSV 存**文件**（getSaveFileName）。
14. **随机 salt 别用 QByteArray::number 单次拼接**（奇数位 hex fromHex 往返失败）→ 两个 number 拼接 32 字符。
15. **ctest -R 匹配用例名非 tag**（"rt-interact" 等 tag 过滤 No tests found 误导一次）。

## 8. 提交与工作区说明

- 切片 28-41 已全部入库（用户侧提交，含 34-36 复合提交 a3f4c95），HEAD=`23d5ec5`，工作区干净。
- 本文档为 V2 阶段正式归档件；V2 后 SRS P0/P1 功能面实现率 100%（P0=79 全闭环，P1 可做项已落地大半，仅架构预留与 P2 留档项未做）。
