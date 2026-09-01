# Phase 4 收口报告 — 上位机 UI 与双轨联调

> 版本：v1.0（2026-09-01）｜切片 18 → 27｜ctest 268/268 全绿｜构建 0 警告

## 1. 范围与目标

Phase 4 对应 ENS-DEV-GUIDE 第 5A 步：上位机 UI 从骨架到实装，并完成与测试台（DeviceSimulator）的双轨联调闭环。

| 5A 步骤 | 内容 | 状态 |
|---|---|---|
| 5.1.1 | 登录 + 主窗口框架 + 7 视图骨架 | ✅ 切片 19 |
| 5.1.2 | 实时曲线（30Hz 批处理 + 降采样） | ✅ 切片 20 |
| 5.1.3 | SBO 安全控制面板 | ✅ 切片 21 |
| 5.1.4 | 渲染优化（OpenGL / High DPI / QSettings） | ✅ 切片 21（i18n 留 V2） |
| 5.1.5 | 双轨联调验证（5C） | ✅ 切片 22 |

## 2. 交付清单（切片 18 → 27）

| 切片 | 主题 | 关键交付 |
|---|---|---|
| 18 | B10 GUI 控制台 | DeviceSimulator 五模块控制台 + Qt5::Svg 接线 + 双模式入口 |
| 19 | 认证 + 主窗框架 | AuthManager 最小版、LoginDialog/SessionLockDialog、MainWindow 7 视图、QSS 暗色主题、**qrc 接入修复** |
| 20 | 实时曲线 | RenderDownsampler 保尖峰降采样、ChannelBuffer 双缓冲、RealtimePlotWidget 30Hz 批处理、8 通道订阅桥 |
| 21 | SBO 面板 + 渲染 | sboStateChanged 信号、SBOControlWidget 按钮态联动、OpenGL 探测、UiDeps 聚合、QSettings 持久化、High DPI |
| 22 | 5C 联调收口 | 连接状态状态栏、断链恢复断言、run_phase4_acceptance.ps1 验收脚本 |
| 23 | 诊断 + 配置 | DiagWidget（链路质量/吞吐曲线）、ConfigWidget（点表/规则/链路三页） |
| 24 | 历史趋势 | IDataAccess 抽象 + queryRange 跨月路由、HistoryTrendWidget（**7 视图收官**） |
| 25 | 长稳烤机 | run_soak_test.ps1（双进程长跑 + 周期采样 + 泄漏/衰减判据） |
| 26 | RBAC V1.6 | 权限点矩阵、checkPermission、环形审计、ScopedAuthGuard、UI 裁剪、活动检测 |
| 27 | 收尾修复 | removeDatabase warning 根因修复（QSqlDatabase 实例存活顺序） |

## 3. 架构现状

```
ens_app (bin/Debug)
├── ui 层（STATIC ens::ui）       —— MainWindow + 7 视图 + 认证对话框 + 主题/图表
├── business 层（SHARED）         —— AlarmEngine / SboStateMachine / AuthManager(RBAC) / DeviceSboGuard
├── datahub 层（SHARED）          —— DataBus / L1SnapshotStore / DownSampler / L2HistoryStore / SQLiteDataAccess(IDataAccess) / BlackBoxManager
├── protocol 层（SHARED）         —— ModbusEngine / PollScheduler / PointTable
└── channel 层（SHARED）          —— TcpChannel / IChannel(ChannelStats)
```

- 依赖注入铁律：ens::ui 经 `UiDeps` 聚合接收数据源（bus/alarm/auth/sbo/channel/pointTable/dataAccess + 回调），不触碰 app 层头。
- 7 视图全实装：总览 / 实时曲线 / 告警中心 / 历史趋势 / 参数配置 / 通信诊断 / SBO 控制。

## 4. 测试基线

| 项 | 数值 |
|---|---|
| ctest | **268/268**（单元 + 集成 + 场景） |
| 构建 | 0 错误 0 警告（MSVC /utf-8） |
| GUI 冒烟 | 登录窗挂起存活（自动化受限，交互留本地） |
| 双进程验收 | run_phase4_acceptance.ps1 6 项断言 PASS（sim_report/连接/采样/月库/黑匣子/规则） |
| 烤机 | run_soak_test.ps1 4min 冒烟 PASS（内存 +0.7MB、CPU <1%、句柄恒定） |

## 5. 验收证据与工具

| 工具 | 用途 | 位置 |
|---|---|---|
| run_phase4_acceptance.ps1 | 双进程 CLI 收口断言 | tools/ |
| run_soak_test.ps1 | 长稳烤机（默认 60min） | tools/ |
| overheat_fast.json 等 4 场景 | 告警/SBO/断链 drill | build/vs2022-debug/test_data/scenarios/ |

本地验收路径：`DeviceSimulator --cli 999 --pointtable <英文点表>` + `ens_app --point-table <英文点表>` → 登录（admin/Admin@123）→ 7 视图逐页验证；operator/Operator@123 验证 RBAC 裁剪。

## 6. 已知遗留（V2 计划）

| 项 | 说明 | 优先级 |
|---|---|---|
| i18n tr() 全量 | 当前无国际化需求，工程机制已就绪 | 低 |
| UI 自动化冒烟 | ens_tests 为 QCoreApplication，QWidget 测试需 QApplication 改造 | 中 |
| 密码哈希 / 登录失败锁定 | NFR-SEC-06（当前明文 + 无失败计数） | 高（安全） |
| 首登强改密 / 用户管理 UI | FR-AUTH-01 完整版 | 中 |
| 告警规则低告警方向 | AlarmRule 缺 direction 字段（SOC 低告警） | 中 |
| 黑匣子/审计可视化 | 文件级可用，UI 展示未做 | 低 |

## 7. 主要坑位沉淀（防回归）

1. **MSVC 中文注释 → /utf-8 必须保留**；TEST_CASE 首参数严格 ASCII（GBK 控制台坑）。
2. **QSqlDatabase::removeDatabase 前必须销毁全部实例**（含隐式共享拷贝），否则 "still in use"。
3. **QStackedWidget 无 replaceWidget**（QStackedLayout 才有）→ removeWidget+addWidget。
4. **AUTOUIC 生成名取 .ui 文件名**（非 class 名）→ .ui 命名与 class 一致。
5. **inline const QString 在 MSVC 命名空间解析异常** → 权限点用 const char*。
6. **PowerShell 5.1 无 BOM UTF-8 脚本按 GBK 解析** → 工具脚本全英文。
7. 降采样桶预算 = target/2（每桶 2 点）才不超限。
8. ChannelBuffer 含锁不可拷贝 → QHash value 用 QSharedPointer。

## 8. 建议提交序列（未提交部分由用户决定）

切片 18~27 均已按提交建议逐个入库（用户侧执行），当前工作区随切片 27 已清空。
