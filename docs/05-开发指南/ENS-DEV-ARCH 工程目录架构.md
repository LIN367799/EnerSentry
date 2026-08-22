# EnerSentry 工程目录架构（apps/ens_app + apps/device_simulator）

> **文档编号**：ENS-DEV-ARCH  
> **版本**：V1.1  
> **日期**：2026-08-19（2026-08-20 增补测试台目录树并更名）  
> **定位**：本文在《ENS-HLD-000 概要设计》《ENS-LLD-000 详细设计总纲》《ENS-DEV-GUIDE 开发步骤指南》及 ENS-LLD-100/200/400/500、ENS-(HLD|LLD|SIM)-SIM 各子模块详细设计基础上，**汇总并落地一份可直接照着建目录、放文件的单仓库双子工程物理代码结构**。  
> **覆盖范围**：`apps/ens_app`（上位机主程序，被测系统）+ `apps/device_simulator`（设备模拟与故障注入测试台，对端）。两工程编译零依赖，仅经统一字节流契约对接；仓库根 `tests/`（三层验证）、`3rdparty/`、`tools/`、`build/`、`bin/` 为两轨共享基础设施。

---

## 0. 一句话总览

单仓库 `EnerSentry` = **两个互不依赖的独立 Qt 工程**：

- **Track A `apps/ens_app`**：上位机主程序（本文主角），C++17 / Qt 5.15 LTS / QCustomPlot / SQLite(WAL)。
- **Track B `apps/device_simulator`**：设备模拟与故障注入测试台（对端），经标准 `IChannel` / `ISlaveTransport` 零改动对接。

主程序内部严格五层架构，**每层一个物理目录 + 一个 CMake Target + 一个 `ens::` 命名空间**。层间只经抽象接口（`IChannel` / `IDataAccess` / `IBusinessEngine` / `IUIController`）与信号槽通信，禁止跨层 `#include` 实现类。

---

## 1. 单仓库总览（EnerSentry/）

```
EnerSentry/                              # 单仓库根
├── CMakeLists.txt                       # 顶层：option 开关两子工程 + vcpkg 依赖收口（见 vcpkg.json + cmake/Ens3rdparty.cmake）
├── apps/
│   ├── ens_app/                         # ★ 上位机主程序（本册主角，被测系统）
│   └── device_simulator/                # 测试台（独立 Qt 工程，对端；完整目录树见 §2.1）
├── 3rdparty/                            # 仅放需 vendored 的本地改版源码（如定制过的库）；常规依赖全走 vcpkg，不在此 vendored（见 §2.2.1）
├── vcpkg.json                           # vcpkg manifest：声明全部依赖（Qt5 / QCustomPlot / Catch2 / nlohmann_json / spdlog / SQLite）
├── cmake/
│   └── Ens3rdparty.cmake                # 依赖唯一收口：集中 find_package（vcpkg 提供）+ target_link_libraries(ens_3rdparty)
├── tests/                               # 三层验证基础设施（两轨共链 ens_tests）
│   ├── CMakeLists.txt                   # add_executable(ens_tests) + catch_discover_tests
│   ├── unit/                            # 纯 C++17 模块单测（Tier 2）
│   │   ├── test_crc16.cpp               #   CRC-16/MODBUS 查表（ENS-LLD-100）
│   │   ├── test_txid_allocator.cpp      #   Transaction ID 位图分配（O(1)）
│   │   ├── test_accumulator.cpp         #   字节流累加器 拆/粘包/溢出
│   │   ├── test_ringbuffer.cpp          #   无锁环形缓冲（越界/回卷）
│   │   ├── test_downsampler.cpp         #   1s/5s/1m 分桶聚合
│   │   └── test_pointtable.cpp          #   点表 resolve/缩放
│   ├── integration/                     # 链入 sim_engine 做 master/slave 回环（Tier 3）
│   │   └── test_modbus_loopback.cpp     #   主程序引擎 ↔ 测试台引擎 回环互逆（CRC 方言一致）
│   └── mock/                            # IChannel Mock 实现，注入脏字节流
│       └── MockChannel.h                #   实现 ens::channel::IChannel，喂坏帧/丢字节
├── tools/
│   └── ci-checks/                       # 总纲 §6.8 静态检查（Clang-Tidy/Cppcheck/AST 扫描）
├── build/                              # ★ CMake 二进制目录（out-of-source，gitignore，不入库）
│   └── apps/ens_app/                   #   ens_app.exe / channel.dll / business.dll
│       └── ...                         #   + 自动生成物：moc_*.cpp / ui_*.h / qrc_*.cpp（见 §2.2.3）
└── bin/                               # ★ 部署产物目录（可选；CMAKE_RUNTIME_OUTPUT_DIRECTORY 目标）
    ├── ens_app.exe                     #   可执行 + 配套动态库，独立可运行（见 §2.2.2）
    ├── channel.dll / business.dll      #   SHARED 模块（若用混合构建）
    └── Qt5Core.dll …                   #   Qt 动态库随 exe 部署（LGPL 合规，§2.2.1）
```

> 注：`tests/`、`3rdparty/`、`tools/` 位于仓库根，服务主程序验证体系；`device_simulator` 的源码不进入主程序编译（NFR-TEST-03）。`build/`、`bin/`、`*.user`、`data/` 全部 gitignore，不入库（详见 §2.2）。

---

## 2. 上位机主程序完整目录树（apps/ens_app/）

```
apps/ens_app/
├── CMakeLists.txt                       # 定义 ens::channel/protocol/datahub/business/ui/app 各 Target
├── include/
│   └── ens/
│       └── export.hpp                   # SHARED 符号导出宏（ENS_CHANNEL_API / ENS_BUSINESS_API）
├── src/
│   ├── channel/                         # L1 通信接入层  ── ens::channel   (SHARED)
│   │   ├── IChannel.h                   # 统一通道抽象接口（<<interface>>）
│   │   ├── SerialChannel.h / .cpp       # RS485/RS232 + RTS 方向控制 + 链路感知
│   │   ├── TcpChannel.h / .cpp          # Modbus TCP Client + 指数退避重连 + KeepAlive/半开识别
│   │   ├── CanChannel.h / .cpp          # SocketCAN / ZLG CAN 统一抽象
│   │   ├── ChannelFactory.h / .cpp      # 通道工厂 + 插件式注册
│   │   ├── ChannelConfig.h              # 通道配置（variant 多态 + SPI 扩展位）
│   │   └── ChannelStats.h              # 原子通信统计 + 60s 滑动窗口（通信质量）
│   │
│   ├── protocol/                        # L2 协议处理层  ── ens::protocol  (STATIC)
│   │   ├── ModbusFrame.h / .cpp         # 帧结构 / 功能码 / 异常码 编解码
│   │   ├── Crc16.h                      # CRC-16/MODBUS 查表（constexpr 256 项）
│   │   ├── ModbusStreamAccumulator.h    # RTU/TCP 字节流累加器（零动态分配，环形覆盖）
│   │   ├── ModbusEngine.h / .cpp        # 流式拼帧状态机 + 解析 + 分发（持有累加器+TxId）
│   │   ├── TransactionIdAllocator.h     # TCP Transaction ID 位图分配（O(1)，inFlight 清理）
│   │   ├── PollScheduler.h / .cpp       # 多链路调度 + 优先级队列 + 三级熔断（含 PROBING）
│   │   └── PointTable.h / .cpp          # 点表解析器：字节序 + 数据类型 + scale/offset + 热加载 RCU
│   │
│   ├── datahub/                         # L3 数据中枢层  ── ens::datahub  (STATIC)
│   │   ├── Sample.h                     # Sample（alignas(16) + 双 static_assert）+ SampleWithMeta
│   │   ├── RingBuffer.h                 # 无锁环形缓冲模板（SPSC + 多消费者 Cursor + epoch 校验）
│   │   ├── L1SnapshotStore.h / .cpp     # L1 内存快照库（稠密数组 + 稀疏 QHash 回退）
│   │   ├── BlackBoxManager.h / .cpp     # 黑匣子快照管理（±30s 锁定 + 异步落盘）
│   │   ├── CriticalSwapFile.h / .cpp    # Critical 级 mmap 即时落盘（槽位/头结构）
│   │   ├── L2HistoryStore.h / .cpp      # L2 历史持久化（WAL + 批量事务 + 双触发）
│   │   ├── IDataAccess.h                # 数据访问抽象接口（<<interface>>，L4/L5 只依赖此）
│   │   ├── SQLiteDataAccess.h / .cpp    # SQLite 实现：多库路由（meta.db 全局 / audit_YYYYMM.db 按月 / history_YYYYMM.db 按月）+ ATTACH 守卫 + WAL + 连接池；用户/审计落库经单一 insertAuditLog/用户接口
│   │   ├── DownSampler.h / .cpp         # 1s/5s/1m 分桶聚合（Max/Min/Avg/Count）
│   │   ├── DataBus.h / .cpp             # 实时数据总线（观察者模式 + 订阅表）
│   │   ├── LifecycleManager.h / .cpp    # 数据生命周期管理（横向，L1 滚动 + L2 过期）
│   │   ├── DataCleaner.h / .cpp         # 过期清理执行器
│   │   └── platform/                    # 跨平台 mmap 抽象层（ADR-20）
│   │       ├── PlatformMMap.h           # IMappedFile 抽象接口 + createMappedFile() 工厂
│   │       ├── Win32MMap.cpp            # Windows：CreateFileMapping/MapViewOfFile
│   │       ├── PosixMMap.cpp            # POSIX：open/mmap(MS_ASYNC)/munmap
│   │       └── CriticalSwapRecovery.cpp # 启动 backup & recreate（句柄锁冲突恢复）
│   │
│   ├── business/                        # L4 业务逻辑层  ── ens::business  (SHARED)
│   │   ├── AlarmEngine.h / .cpp         # 告警引擎（迟滞判定 + 风暴抑制 + 延时确认）
│   │   ├── SBOStateMachine.h / .cpp     # SBO 状态机（Select→Armed(5s)→Operate）
│   │   ├── DeviceSboGuard.h / .cpp      # 设备级二维 key 细粒度锁（ADR-23）
│   │   ├── BusinessStateMachine.h / .cpp# Station/Device/Point 配置/运行/统计三态
│   │   ├── AuthManager.h / .cpp         # RBAC 三级角色 + bcrypt(cost=12) 哈希；登录/失败锁定经 IDataAccess 双写「内存 + meta.db」(NFR-SEC-06 重启不丢锁)
│   │   ├── SessionManager.h / .cpp      # 会话管理（仅驻内存 QHash，不落库；sweepExpiredSessions 周期驱逐 15min 超时）；登录态不跨重启保持
│   │   ├── ConfigManager.h / .cpp       # 点表/阈值/链路热加载驱动
│   │   ├── QueryEngine.h / .cpp         # 历史查询引擎（跨月 ATTACH + 降采样查询/导出）
│   │   └── DiagManager.h / .cpp         # 诊断管理（通信质量/报文抓取）
│   │
│   ├── ui/                              # L5 UI 视图层  ── ens::ui       (STATIC)
│   │   ├── common/                      # 公用组件与样式（跨视图复用）── ens::ui::common
│   │   │   ├── theme.qss                # 全局暗色主题样式表（主样式；AUTORCC 经 resources.qrc 内嵌为 :/qss/theme.qss）
│   │   │   ├── Theme.h / .cpp           # 主题管理器：加载 theme.qss + 暴露调色板常量（供 QCustomPlot 等 qss 无法着色的绘制复用）
│   │   │   ├── widgets/                 # 可复用自定义控件（多视图共用，不绑定具体业务）
│   │   │   │   ├── ValueLabel.h / .cpp          # 带单位/阈值着色的测点数值标签
│   │   │   │   ├── AlarmBadge.h / .cpp          # 告警角标（各级别配色取 Theme 调色板）
│   │   │   │   ├── TrendChart.h / .cpp          # 基于 QCustomPlot 的通用趋势图（总览/实时/历史三视图共用）
│   │   │   │   ├── LoginWidget.h / .cpp         # 共享登录表单（用户名/密码 + 首登强改密），被 LoginDialog 与 SessionLockDialog 复用
│   │   │   │   └── DrillDownNavigator.h / .cpp  # 三级钻取导航器（总览/明细共用）
│   │   │   └── resources/               # 公共图标/图片源文件，经 resources.qrc 内嵌
│   │   │       ├── icons/               # svg/png 图标
│   │   │       └── images/              # 背景/装饰图
│   │   ├── LoginDialog.h / .cpp / .ui   # 启动登录模态框（FR-AUTH-01）：内嵌 LoginWidget；失败计数触发账户锁定（NFR-SEC-06）
│   │   ├── SessionLockDialog.h / .cpp / .ui  # 会话超时锁屏（FR-AUTH-05）：复用 LoginWidget；解锁后恢复 MainWindow
│   │   ├── MainWindow.ui                # ← Qt Designer 表单（AUTOUIC 生成 ui_MainWindow.h 入 build/，不入库）
│   │   ├── MainWindow.h / .cpp          # 主框架 + 暗色主题 + 7 视图容器 + 三级钻取
│   │   ├── OverviewWidget.ui            # ← 各视图 Widget 的 .ui 与 .h/.cpp 共位（约定见 §2.2.3）
│   │   ├── OverviewWidget.h / .cpp      # ① 电站总览（含 DrillDownNavigator）
│   │   ├── RealtimeChartWidget.h / .cpp # ② 实时曲线（图表容器）
│   │   ├── RealtimePlotWidget.h / .cpp  # ② 单通道绘图（QTimer 30/60Hz + 降采样 + rpQueuedReplot）
│   │   ├── RenderDownsampler.h / .cpp   # UI 层 Min-Max 桶降采样（区别于 L2 落盘降采样）
│   │   ├── OpenGLDetector.h / .cpp      # OpenGL 后端探测 + 软件渲染回退
│   │   ├── AlarmCenterWidget.h / .cpp   # ③ 告警中心
│   │   ├── HistoryTrendWidget.h / .cpp  # ④ 历史趋势
│   │   ├── ConfigWidget.h / .cpp        # ⑤ 参数配置
│   │   ├── DiagWidget.h / .cpp          # ⑥ 通信诊断
│   │   ├── SBOControlWidget.h / .cpp    # ⑧ SBO 控制台（dev guide 中称 ControlPanel）
│   │   └── resources.qrc                # 聚合 common/resources（图标/图片）+ 默认配置 + theme.qss（AUTORCC → qrc_resources.cpp，入 build/）
│   │
│   └── app/                             # 应用基础设施   ── ens::app      (EXECUTABLE)
│       ├── main.cpp                     # 入口：qRegisterMetaType 集中注册 + 线程装配 + 配置加载
│       └── ThreadManager.h / .cpp       # 线程拓扑编排（采集/解析/持久化/告警/UI 准备线程）
│
├── config/                              # 默认配置（可内嵌 :/qrc，也可随 exe 部署覆盖）
│   ├── channels.json                    # 通信链路配置（TCP 5020 / 虚拟串口 RTU）
│   ├── pointtable.json                  # 点表配置（寄存器→工程值，权威地址）
│   ├── alarm_rules.json                 # 告警阈值配置
│   └── runtime.json                     # 黑匣子/fsync/渲染等运行期参数
│
└── data/                               # 运行时生成产物（gitignore，不入库）
    ├── meta.db                          # 账号/角色/全局 KV（DBDD §4.6）：users(pw=bcrypt 哈希 + gensalt 盐 + locked_until) / global_kv；首跑建库并 seeded 默认 admin（FR-AUTH-03）
    ├── audit/YYYYMM/audit_YYYYMM.db     # 审计日志库（DBDD §4.5，按月）：login/control/config 操作留痕（user/role/action/result）；无 UPDATE/DELETE，不可篡改，仅 admin 可见
    ├── history/YYYYMM/data_YYYYMM.db   # 按月分库历史数据（1s/5s/1m 降采样表）
    ├── blackbox.db                      # 黑匣子快照表（100ms 原始高频，永久保留）
    └── critical_swap.dat               # Critical 级 mmap 即时落盘交换文件（100MB 循环）
```

> 注：`build/`（CMake 二进制目录，含 `moc_*.cpp` / `ui_*.h` / `qrc_*.cpp` 自动生成物）、`bin/`（部署产物）、`*.user`（Qt Creator 用户文件）、`data/` 全部 **gitignore，不入库**。源码树只留手写 `.h / .cpp / .ui / .qrc / .json`，生成物与产物一律在 `build/` 或 `bin/`（详见 §2.2）。

---

## 2.1 测试台（apps/device_simulator）完整目录树 —— 测试程序

> 测试台即「设备模拟与故障注入测试程序」，是主程序的对端。其目录风格与主程序（§2）一致：**每层一个物理目录 + 一个 CMake Target**；但内部按 `core / sim / gui` 三分（非主程序的五层），且 `sim` 引擎为**纯 C++17 零 Qt**，可独立被 `tests/integration` 链入做回环单测（ENS-SIM-IMP §1/§5/§9）。

```
apps/device_simulator/                      # ★ 测试程序（设备模拟与故障注入，对端）
├── CMakeLists.txt                          # add_executable(DeviceSimulator) + add_library(sim_engine STATIC)
├── src/
│   ├── main_gui.cpp                        # QApplication + SimulatorMainWindow 入口（Qt 5.15 Widgets）
│   ├── core/                               # 公共契约层  ── ens::core  (纯 C++17 零 Qt)
│   │   ├── point_table.h                   # PointTableEntry/RegisterType/DataType/ByteOrder（与 ENS-LLD-SIM 逐字节一致，自包含拷贝）
│   │   ├── crc16.h / .cpp                  # CRC-16/MODBUS（poly 0xA001, init 0xFFFF，与主程序互逆）
│   │   └── mbap.h / .cpp                   # MBAP 头 parse/emit（大端，transactionId 原样透传）
│   │
│   ├── sim/                                # 仿真引擎层  ── ens::sim  (纯 C++17 零 Qt → sim_engine)
│   │   ├── ISlaveTransport.h               # 抽象传输层（TCP 监听 / RTU 从站共用，<<interface>>）
│   │   ├── modbus_slave.h / .cpp           # ModbusSlaveEmulator：持双 transport，共用 RegisterBank
│   │   ├── modbus_tcp_server.h / .cpp      # ModbusTcpServer：原生 socket 监听 5020（FC03/04/06 回显）
│   │   ├── rtu_slave_port.h / .cpp         # RtuSlavePort：虚拟串口 RTU 从站 + CRC-16 校验
│   │   ├── register_bank.h / .cpp          # RegisterBank：RCU 快照库（shared_ptr<const> 原子替换）
│   │   ├── point_generator.h / .cpp        # PointGenerator：物理演化（evolveBms/Pcs/Meter/Aux）
│   │   ├── sim_config.h / .cpp             # SimConfig：加载点表/端点/tickMs/seed/exportLogPath
│   │   ├── fault_injector.h / .cpp         # FaultInjector + FaultSession：五类故障状态机
│   │   ├── scenario_script.h / .cpp        # ScenarioScript：解析 scenarios/*.json 驱动故障注入
│   │   └── simulator_engine.h / .cpp       # SimulatorEngine：DataTick→SlaveIO→FaultInjector 编排
│   │
│   └── gui/                                # Qt 5.15 前端层（薄前端，仅 DeviceSimulator.exe 链接，30Hz 轮询快照）
│       ├── common/                        # 测试台 GUI 公用组件与样式（命名约定镜像主程序 src/ui/common，避免重复造轮子）
│       │   ├── theme.qss                  # 暗色主题样式表（经 resources.qrc 内嵌为 :/qss/theme.qss）
│       │   ├── Theme.h / .cpp             # 主题管理器：加载 qss + 暴露调色板常量（log_view 按 level 着色等复用）
│       │   ├── widgets/                   # 跨面板复用的通用控件（按实际复用情况增删）
│       │   │   ├── StatusLed.h / .cpp     # 链路/从站在线状态指示灯（多面板共用）
│       │   │   └── LogLineDelegate.h/.cpp # 事件日志按 level 着色的条目代理（log_view 复用）
│       │   └── resources/                 # 公共图标/图片源文件，经 resources.qrc 内嵌
│       │       ├── icons/
│       │       └── images/
│       ├── main_window.h / .cpp            # SimulatorMainWindow：菜单/工具栏/状态栏
│       ├── main_window.ui                  # ← Qt Designer 表单（AUTOUIC → build/ui_main_window.h，不入库）
│       ├── register_view.h / .cpp / .ui    # 设备树（23 从站）+ 实时寄存器表
│       ├── fault_panel.h / .cpp / .ui      # 故障注入控制台
│       ├── scenario_runner.h / .cpp / .ui  # 场景运行器（进度条）
│       ├── log_view.h / .cpp / .ui         # 事件日志视图（按 level 着色、可导出）
│       ├── config_panel.h / .cpp / .ui     # 配置面板（tcp/rtu 端点、tickMs/seed/exportLogPath）
│       └── resources.qrc                   # 聚合 common/resources + theme.qss（AUTORCC → qrc_resources.cpp，入 build/）
│
├── config/
│   └── sim_config.json                     # 测试台默认配置（端点 / tickMs / seed / 导出路径）
└── data/                                  # 运行期导出（gitignore，不入库）
    ├── sim_events.jsonl                    # 每事件一行（B9 产出，NFR-TEST-02）
    └── sim_report.json                     # 场景结束报告（result ∈ {PASS,FAIL,INCONCLUSIVE}）
```

> **测试台 GUI 公用资产与主程序同构**：测试台虽是「薄前端」，但其 `src/gui/` 同样会有跨面板复用的控件与暗色主题，因此**照搬主程序 `src/ui/common/` 的约定**——`theme.qss` + `Theme` 管理器 + `widgets/`（通用控件）+ `resources/`（图标图片），并经 `src/gui/resources.qrc` 内嵌。两条铁律与主程序一致：① `moc_/ui_/qrc_*` 仅存 `build/`、不入库；② ≥2 个面板共用的控件/样式一律进 `common/`，不在单面板 `.cpp` 里各写一份。测试台 GUI 通常**不绘图表**（无 QCustomPlot），故 `Theme` 的 C++ 调色板主要用于 `log_view` 的 level 着色与状态灯，比主程序更轻。

### 2.1.1 CMake Target 划分（测试台）

| Target | 类型 | 源码 | 链接方 | 说明 |
|--------|------|------|--------|------|
| `sim_engine` | **STATIC** | `src/core/*` + `src/sim/*` | `DeviceSimulator` + `ens_tests` | 纯 C++17 零 Qt，可被主程序 `tests/integration` 回环单测链入（NFR-TEST-03 例外：仅**测试期**依赖） |
| `sim_gui` | STATIC / OBJECT | `src/gui/*` | `DeviceSimulator` | 仅 GUI 前端，含 `.ui` 表单 |
| `DeviceSimulator` | **EXECUTABLE** | `src/main_gui.cpp` + link `sim_engine` `sim_gui` + `Qt5::Widgets` | — | 产出 `DeviceSimulator.exe`，无 headless、无 SimulationMode（ENS-SIM-IMP §9） |

- **零 Qt 边界**：`src/core` / `src/sim` 严禁 `#include <Q...>`；用 `std::thread` + `std::mutex`/`std::condition_variable` 做并发，不依赖 Qt 信号槽。GUI 层才用 Qt 事件循环消费 RCU 快照（ENS-SIM-IMP §9/§10）。
- **与主程序零编译依赖**：`DeviceSimulator` 不进入 `ens_app` 编译；唯一运行期对接是字节流契约（§7）。`sim_engine` 仅被 `tests/` 链入做回环测试，不影响主程序运行期产物。

### 2.1.2 测试台目录 ↔ 主程序目录 对应（镜像双栈部署）

| 主程序（被测） | 测试台（对端） | 对接契约 |
|----------------|---------------|----------|
| `src/channel/TcpChannel` | `src/sim/modbus_tcp_server` | Modbus TCP `127.0.0.1:5020`（BMS/PCS/电表，高频） |
| `src/channel/SerialChannel` | `src/sim/rtu_slave_port` | 虚拟串口 RTU（液冷/消防辅机，RS485） |
| `src/protocol/ModbusEngine` | `src/sim/register_bank` + `point_generator` | 寄存器→工程值映射（同 `sim_pointtable_sample.json`） |
| `src/business/SboStateMachine` | `src/sim/modbus_slave`（FC05/06/10 回显） | SBO Select→Armed→Operate 全链路 |
| `tests/integration/test_modbus_loopback` | `sim_engine` | master/slave 回环单测（CRC 方言互逆） |

---

## 2.2 三方库 / 编译产物 / Qt 资源文件（.ui / .qrc）落点约定

> 本节补全 §1/§2 树里「没展开但 Qt 工程必碰」的三类东西：**依赖从哪来、产物放哪、`.ui` 表单搁哪**。全部与开发指南 §6「`ens_3rdparty` INTERFACE 库 + vcpkg（仓库根 vcpkg.json）」「`CMAKE_RUNTIME_OUTPUT_DIRECTORY → bin/`」的既有约定一致。

### 2.2.1 三方库（依赖统一经 vcpkg 引入）—— 不散落进 src/

- **总原则**：第三方依赖**统一经 vcpkg 引入**（manifest 模式：仓库根 `vcpkg.json` 声明依赖，`CMAKE_TOOLCHAIN_FILE` 指向 vcpkg 工具链），绝不在 `apps/ens_app/src/` 里塞 Qt / qcustomplot 源码。所有依赖经 `ens_3rdparty` INTERFACE 库 `target_link_libraries(... PRIVATE ens_3rdparty)` 统一透传（开发指南 §6 / ENS-DEV-BOOT §4）。
- **依赖清单（全经 vcpkg）**：
  - `qt5-base`、`qt5-serialport`（Qt5 Core/Gui/Widgets/SerialPort/Network）—— LGPL 必须 SHARED，vcpkg 默认即动态链接，正好契合 §4 决策。
  - `qcustomplot`（绘图）、`catch2`（单测）、`nlohmann-json`（JSON）、`spdlog`（日志）、`sqlite3`（如需独立 SQLite，否则用 Qt `QSql`）。
- **仓库根 `3rdparty/` 仅放一类**：确需 vendored 的本地改版（如定制过的 qcustomplot 源码）→ `3rdparty/<libname>/`，由顶层 `CMakeLists.txt` 用 `add_subdirectory` 或 `FetchContent` override 接入，**仍不进 `src/`**；常规依赖不在此 vendored。
- **LGPL 红线（⚠ 重要）**：Qt 是 LGPL 协议，必须 **SHARED 动态链接**。部署时 Qt 动态库（Qt5Core.dll / Qt5Widgets.dll / …）随 `ens_app.exe` 落在 `bin/`，**严禁把 Qt 静态编入 exe** 以规避 LGPL 反向工程义务——这条与 §4 的 SHARED 决策互为表里。

### 2.2.2 编译产物（build / bin）—— out-of-source，整目录 gitignore

- **`build/`**（仓库根，out-of-source）：直接 `cmake -S . -B build` 生成，**整目录 gitignore**，不入库。里面包含：
  - `CMakeCache.txt`、各 `*.cmake` 脚本、各 Target 的 `.obj/.o`、链接出的 `ens_app.exe` / `channel.dll` / `business.dll`（默认在 `build/apps/ens_app/`）。
  - **Qt 自动生成物**：`moc_*.cpp`（元对象）、`ui_*.h`（uic 生成）、`qrc_*.cpp`（rcc 生成）——见 §2.2.3。
- **`bin/`**（仓库根，可选）：设 `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 指向 `bin/` 时，最终 `ens_app.exe` + `channel.dll` + `business.dll` + Qt DLL **集中于此**，可直接整目录拷走运行。Linux 需设 `RPATH=$ORIGIN` 让 exe 自动找同目录 `.so`（开发指南 §6）。
- **Qt Creator 用户文件**：`*.pro.user` / `ens_app.user` 等**gitignore**（每人本地状态，不共享）。
- **`.gitignore` 关键行参考**：
  ```gitignore
  build/            # CMake 二进制目录（含 moc/ui/qrc 自动生成物）
  bin/              # 部署产物
  *.user            # Qt Creator 用户配置
  apps/ens_app/data/  # 运行期数据（同 §2 树 data/）
  ```
- 运行期数据 `data/`（§2 树末）同样 gitignore，仅保留空目录占位或 `.gitkeep`。

### 2.2.3 Qt 资源文件（.ui / .qrc / moc）—— 表单共位，生成物全在 build/

**`.ui` 文件（Qt Designer 表单）—— 与对应 Widget 共位，放在 `src/ui/`**

- 约定：每个视图 Widget 的 `.ui` 紧挨其 `.h/.cpp`。例：`src/ui/MainWindow.ui` 与 `MainWindow.h/.cpp` 同目录（已写入 §2 树）。
- 理由：CMake **`AUTOUIC`** 看到 `MainWindow.h` 里有 `#include "ui_MainWindow.h"`，会自动在 `src/ui/` 找到同名 `MainWindow.ui`，把 `ui_MainWindow.h` 生成进 `build/`——**无需手写任何 uic 规则**，共位最省心。
- 若 Widget 多、想集中管理，可建 `src/ui/forms/` 统一放 `.ui`；此时需在 CMake 用 `set_source_files_properties(xxx.ui PROPERTIES AUTOUIC ON)` 或保证头文件 include 路径正确。**默认仍推荐共位**。
- 在 `MainWindow.cpp` 里：`#include "ui_MainWindow.h"`，并在 `MainWindow(QWidget* p=nullptr) : ui(new Ui::MainWindow), QWidget(p) { ui->setupUi(this); }` 接管表单。

**`.qrc` 文件（资源）—— 已在 `src/ui/resources.qrc`**

- `AUTORCC` 自动把 `resources.qrc` 编译成 `qrc_resources.cpp` 进 `build/`；C++ 内用 `:/qrc/...` 路径访问暗色主题 QSS / 图标 / 内嵌默认配置。
- 若配置/图标想分层，可再加 `src/ui/icons.qrc` 等，同理共位、AUTORCC 接管。

**`.qss` 文件（Qt 样式表 / 暗色主题）—— 放 `src/ui/common/theme.qss`，经 `resources.qrc` 内嵌**

- 全局样式表 `theme.qss` 放在 `src/ui/common/`（见 §2 树），由 `resources.qrc` 编译进二进制，运行时路径 `:/qss/theme.qss`。**禁止**把大段样式写死在各视图 `.cpp` 里——统一定义在 `theme.qss`，避免多视图样式漂移。
- 启动期由 `Theme` 管理器一次性 `QApplication::setStyleSheet(Theme::qss())` 应用到整个 App；视图/控件用 `objectName` / 自定义属性（如 `property: alarmLevel`）在 qss 里命中规则，而非在 C++ 里逐控件 `setStyleSheet()`。
- **为什么还需要 `Theme.h/.cpp`（C++ 调色板常量）**：QCustomPlot 的画笔/画刷/网格线（pen/brush/grid）**QSS 无法着色**，必须直接用颜色值。因此 `Theme` 同时把暗色调色板（背景色、网格色、各级别告警色、曲线配色）以 C++ 常量/`QColor` 形式暴露，供 `RealtimePlotWidget`/`TrendChart`/`RenderDownsampler` 等绘图代码与 qss 主题保持一致。即：**QSS 管控件外观，C++ 调色板管图表绘制，二者同源、由 `Theme` 统一收口**。
- 测试台 `apps/device_simulator` 同理：其 GUI 公用样式已纳入 `src/gui/common/theme.qss`，由 `src/gui/resources.qrc` 内嵌（结构见 §2.1，与主程序 `src/ui/common` 同构）。

**`moc_*.cpp`（元对象代码）—— 全自动，绝不手写**

- 所有带 `Q_OBJECT` 的类（`MainWindow`、`RealtimePlotWidget`、`SboStateMachine`、`ModbusEngine` 等）由 CMake **`AUTOMOC`** 自动生成 `moc_<class>.cpp` 进 `build/`。源码里**不要手写**这些文件，也**不要把它们提交进库**。

**铁律**：`moc_* / ui_* / qrc_*` 三类生成物**一律不入库**，只在 `build/`；源码树只保留你手写的 `.h / .cpp / .ui / .qrc / .json`。

> 测试台 `apps/device_simulator` 同理：`src/gui/*.ui` 与对应 `.h/.cpp` 共位，`MainWindow.ui` 等生成物进 `build/`，不入库（ENS-SIM-IMP §10）。

---

## 3. 目录逐层职责说明

### 3.1 `src/channel/`（L1，SHARED，命名空间 `ens::channel`）
唯一职责：把 RS485 串口 / Modbus TCP / CAN 三类物理介质**统一抽象成字节流通道**。上层协议引擎只依赖 `IChannel` 接口，不感知介质；新增通道类型零改动协议层。
- `IChannel.h` 为纯虚接口，SHARED 导出宏 `ENS_CHANNEL_API` 标注；`SerialChannel`/`TcpChannel`/`CanChannel` 为三种实现。
- `ChannelStats.h` 全 `std::atomic`，采集线程高频更新，供诊断模块 `getStats()` 读取。
- **铁律**：协议层禁止直接 `#include <QSerialPort>` / `<QTcpSocket>`（CI 头文件包含校验，总纲 §6.8）。

### 3.2 `src/protocol/`（L2，STATIC，命名空间 `ens::protocol`）
协议语义层：Modbus RTU/TCP 帧构建/解析、查表 CRC-16、字节流拼帧状态机、点表寄存器→工程值映射、多链路轮询调度与 RS485 半双工串行保护。
- `ModbusEngine` 持有 `ModbusStreamAccumulator` + `TransactionIdAllocator`，经 `IChannel` 收发。
- `PollScheduler` 实现三级熔断 `HEALTHY→DEGRADED→ISOLATED→PROBING`（ADR-13 / ADR-LLD-10）。
- `PointTable` 支持 RCU 热加载（同 `sim_pointtable_sample.json` 逐字节一致）。

### 3.3 `src/datahub/`（L3，STATIC，命名空间 `ens::datahub`）
实时数据缓存、分级存储、降采样、黑匣子、数据总线。
- `Sample` 必须 `alignas(16)` + 双 `static_assert`（ADR-08/18）。
- `RingBuffer` 无锁、幂为 2 容量、多消费者游标 + epoch/sequence 帧完整性校验（总纲 §6.1）。
- `CriticalSwapFile` + `platform/` 实现跨平台 mmap 与断电前 30s 高频数据保护（ADR-14/20/21）。
- `SQLiteDataAccess` 按月分库 + `ATTACH` RAII 守卫 + 只读连接池（ADR-09/15）。
- `LifecycleManager`/`DataCleaner` 实现数据生命周期横向能力（⑩）。

### 3.4 `src/business/`（L4，SHARED，命名空间 `ens::business`）
告警判定、SBO 控制、RBAC、配置管理、历史查询、诊断。
- `SBOStateMachine` + `DeviceSboGuard` 实现 Select→Armed→Operate 与断线/超时自动清锁（FR-CTRL-07 / ADR-23）。
- `AlarmEngine` 风暴抑制 `MAX_PENDING_STORM=2000` + `droppedCount` 原子（ADR-10）。
- 全部经 `IDataAccess` 接口访问存储，经信号槽驱动 UI，**禁止**直接访问通道或裸 Qt SerialPort。

### 3.5 `src/ui/`（L5，STATIC，命名空间 `ens::ui`）
7 大视图 + 主框架 + 暗色主题。
- **铁律（ADR-22）**：严禁数据到达即 `replot()`；统一 `QTimer 30/60Hz` 批处理 + 降采样 + `rpQueuedReplot`；每通道 ≤2000 点 / ≤1920px。
- `OpenGLDetector` 无独显自动回退软件渲染；`RenderDownsampler` 为 UI 层 Min-Max 桶降采样（区别于 L2 落盘降采样）。
- `SBOControlWidget`（dev guide 中称 `ControlPanel`）绑定 `SBOStateMachine::sboStateChanged`，Armed 期间禁用其他操作。
- `LoginDialog` / `SessionLockDialog`（均 `ens::ui`）：启动登录（FR-AUTH-01）与超时锁屏（FR-AUTH-05），二者内嵌 `common/widgets/LoginWidget` 共享表单（满足「≥2 面板复用进 `common/`」铁律）。`main.cpp`（`ens::app`）启动顺序：`LoginDialog::exec()` 成功 → 构造并显示 `MainWindow`；登录态/会话由 L4 `AuthManager`/`SessionManager`（ENS-LLD-403）经业务层抽象注入，UI 绝不直接接触密码哈希/账户存储（与 LLD-500「UI 不碰 SQLite」铁律一致）。账户档案落 `data/meta.db`（`users` 表，bcrypt 哈希 + `locked_until` 双写持久化），操作审计落 `data/audit/YYYYMM/audit_YYYYMM.db`（DBDD §4.5/§4.6）。
- `common/`（命名空间 `ens::ui::common`）：跨视图复用的**公用资产**集中地——`theme.qss` + `Theme` 调色板常量（全局暗色主题单一收口）、`widgets/`（`ValueLabel`/`AlarmBadge`/`TrendChart`/`DrillDownNavigator` 等不绑定具体业务的通用控件）、`resources/`（图标/图片）。原则：**凡是 ≥2 个视图共用的控件或样式，一律进 `common/`，禁止在单个视图里各写一份**。

### 3.6 `src/app/`（应用基础设施，EXECUTABLE，命名空间 `ens::app`）
- `main.cpp`：`qRegisterMetaType` 集中注册跨线程类型（`Sample`/`ModbusResponse`/`AlarmEvent`/`SboState`/`ControlCmd`/`BlackBoxSnapshot`），否则 `QueuedConnection` 静默丢弃。
- `ThreadManager` 编排线程拓扑（采集/解析/持久化/告警/UI 准备），`try-catch` 包裹 `run()` 保 7×24（NFR-REL-01）。

### 3.7 `include/ens/export.hpp`（公共契约）
SHARED 模块符号导出宏，统一兼容 MSVC `__declspec` 与 GCC/Clang `visibility`。仅 `ens::channel` / `ens::business` 使用；STATIC 模块严禁引入。

### 3.8 `config/`（运行时配置）
`channels.json` / `pointtable.json` / `alarm_rules.json` / `runtime.json` 点表驱动、阈值、链路、黑匣子参数全部配置化，支持热加载（FR-CFG-04/06）。

### 3.9 `tests/`（三层验证，见仓库根）
- `unit/`：Tier 2 纯 C++17 模块单测（`test_crc16` / `test_txid_allocator` / `test_accumulator` / `test_ringbuffer` / `test_downsampler` / `test_pointtable`）。
- `integration/`：Tier 3 链入 `sim_engine` 做 Modbus master/slave 回环（`test_modbus_loopback.cpp`）。
- `mock/MockChannel.h`：实现 `ens::channel::IChannel`，直接注入脏字节流验证引擎鲁棒性。

### 3.10 `tools/ci-checks/`
总纲 §6.8 固化规则：STATIC 误用导出宏、UI 槽函数阻塞 I/O、L5 包含实现类头文件、双 `static_assert`、mmap API 黑名单、RS485 重试次数。失败阻断合入。

---

## 4. 五层架构 ↔ CMake Target ↔ 命名空间 ↔ 构建类型 映射表

| 架构层 | 目录 | CMake Target | 命名空间 | 构建类型 | 核心类（部分） | 部署产物 |
|--------|------|--------------|----------|----------|----------------|----------|
| L1 通信接入 | `src/channel/` | `ens::channel` | `ens::channel` | **SHARED** | `IChannel`/`SerialChannel`/`TcpChannel`/`CanChannel` | `channel.dll` |
| L2 协议处理 | `src/protocol/` | `ens::protocol` | `ens::protocol` | STATIC | `ModbusEngine`/`PollScheduler`/`PointTable` | 内联进 exe |
| L3 数据中枢 | `src/datahub/` | `ens::datahub` | `ens::datahub`(:`:platform`) | STATIC | `RingBuffer`/`L1SnapshotStore`/`SQLiteDataAccess` | 内联进 exe |
| L4 业务逻辑 | `src/business/` | `ens::business` | `ens::business` | **SHARED** | `AlarmEngine`/`SBOStateMachine`/`AuthManager`/`QueryEngine` | `business.dll` |
| L5 UI 视图 | `src/ui/` | `ens::ui` | `ens::ui` | STATIC | `MainWindow`/`RealtimePlotWidget`/`SBOControlWidget` | 内联进 exe |
| 应用基础设施 | `src/app/` | `ens::app`（EXECUTABLE） | `ens::app` | EXECUTABLE | `ThreadManager` | `ens_app.exe` |

> **混合构建判定（HLD §2.6.5）**：热路径（protocol/datahub/ui）→ STATIC；因站而异需热替换（channel/business）→ SHARED；LGPL 合规强制 SHARED。切换仅改 CMake 变量，业务代码零改动（前提是接口已纯虚解耦）。
>
> **链接传递陷阱**：STATIC 依赖 SHARED 须用 `PUBLIC`（如 `target_link_libraries(ens_protocol PUBLIC ens::channel)`），否则 exe 链接报 unresolved external。

---

## 5. 关键源文件清单（按模块，可直接建空文件占位）

| LLD 子模块 | 落地产物（文件） | Target / 类型 |
|-----------|------------------|---------------|
| ENS-LLD-101~103（L1） | `IChannel.h` `SerialChannel.*` `TcpChannel.*` `CanChannel.*` `ChannelFactory.*` `ChannelConfig.h` `ChannelStats.h` | `ens::channel` / SHARED |
| ENS-LLD-201~203（L2） | `ModbusFrame.*` `Crc16.h` `ModbusStreamAccumulator.h` `ModbusEngine.*` `TransactionIdAllocator.h` `PollScheduler.*` `PointTable.*` | `ens::protocol` / STATIC |
| ENS-LLD-301~306（L3） | `Sample.h` `RingBuffer.h` `L1SnapshotStore.*` `BlackBoxManager.*` `CriticalSwapFile.*` `L2HistoryStore.*` `IDataAccess.h` `SQLiteDataAccess.*` `DownSampler.*` `DataBus.*` `LifecycleManager.*` `DataCleaner.*` `platform/{PlatformMMap.h,Win32MMap.cpp,PosixMMap.cpp,CriticalSwapRecovery.cpp}` | `ens::datahub` / STATIC |
| ENS-LLD-401~406（L4） | `AlarmEngine.*` `SBOStateMachine.*` `DeviceSboGuard.*` `BusinessStateMachine.*` `AuthManager.*` `SessionManager.*` `ConfigManager.*` `QueryEngine.*` `DiagManager.*` | `ens::business` / SHARED |
| ENS-LLD-501~508（L5） | `MainWindow.*` `OverviewWidget.*` `RealtimeChartWidget.*` `RealtimePlotWidget.*` `RenderDownsampler.*` `OpenGLDetector.*` `AlarmCenterWidget.*` `HistoryTrendWidget.*` `ConfigWidget.*` `DiagWidget.*` `SBOControlWidget.*` `resources.qrc` | `ens::ui` / STATIC |
| ENS-LLD-801（基础设施） | `main.cpp` `ThreadManager.*` | `ens::app` / EXECUTABLE |

---

## 6. 构建类型与部署产物

| 模式 | 产物 | 部署文件数 | 适用阶段 |
|------|------|-----------|----------|
| 基线（全 STATIC） | `ens_app.exe`（~50MB） | 1 个文件 | 开发 / 单站点交付 |
| 混合（STATIC + SHARED） | `ens_app.exe`（~40MB）+ `channel.dll` + `business.dll` | 3 个文件 | 多站点生产部署 |

- SHARED 模式：`CMAKE_RUNTIME_OUTPUT_DIRECTORY` → `bin/`；Linux 设 `RPATH=$ORIGIN`，让 exe 自动找到同目录 `.so`。
- 第三方依赖统一经 `ens_3rdparty` INTERFACE 库封装（Qt5::Core/Widgets/SerialPort/Network、qcustomplot、nlohmann_json、SQLite::SQLite3、spdlog），统一经 vcpkg（仓库根 vcpkg.json manifest + cmake/Ens3rdparty.cmake）引入，禁止源码散落。

---

## 7. 与测试台 `apps/device_simulator` 的边界（零依赖）

> 测试台完整目录树见 §2.1；其 CMake Target 划分（sim_engine / sim_gui / DeviceSimulator）见 §2.1.1。

- **主程序编译不依赖测试台源码**（NFR-TEST-03）。两轨仅经统一字节流契约对接：
  - 主程序 `IChannel`（TCP Client / Serial）↔ 测试台 `ISlaveTransport`（TCP Server / RTU Slave）。
  - 双链路同时启用：Modbus TCP `127.0.0.1:5020`（BMS/PCS/电表）+ 虚拟串口 RTU（液冷/消防辅机），完美镜像主程序"高频走 TCP、辅机走 RS485"双栈部署。
- 唯一直连点：`tests/integration/test_modbus_loopback.cpp` 链入 `sim_engine`（测试台引擎，纯 C++17 零 Qt）做 master/slave 回环单测——这是**测试期**依赖，不影响主程序运行期产物。
- 测试台自身为独立 Qt 5.15 Widgets 工程（`DeviceSimulator.exe` + 内部 `src/sim` 零 Qt 引擎 + `src/gui` 薄前端），无 headless、无 SimulationMode。

---

## 8. 设计演进说明（必读，避免按旧图建错目录）

- **旧布局（HLD §2.5 V1.5）**：单顶层 `src/{channel,protocol,datahub,business,ui,app}` + 顶层 `simulator/`，示例用 Qt6/QSql。这是**早期骨架示意**，早于"单仓库双子工程"决策。
- **现行布局（ENS-DEV-GUIDE V4.0，最终敲定）**：主程序收敛为 **`apps/ens_app/`** 下 `src/...`；测试台独立为 **`apps/device_simulator/`**；技术栈落地 **Qt 5.15 LTS**（非 Qt6）。
- **结论**：**以本文第 2 节（主程序）与第 2.1 节（测试台）树为准**建目录。HLD §2.5 中的 `simulator/` 顶层目录与 Qt6 写法视为已被双子工程决策与 Qt 5.15 选型取代；类名、接口名、ADR 编号、Sample 结构、CRC 算法、熔断阈值等核心契约不变。

---

## 附：照着动手的 3 条铁律（来自 ENS-DEV-GUIDE）

1. **层间只经抽象接口 + 信号槽**，禁止跨层 `#include` 实现类（主程序内部同理）。
2. **跨线程传自定义类型必须 `qRegisterMetaType`**（否则信号静默丢弃，最难查的 bug 之一）。
3. **任何超时/间隔用 `std::chrono::steady_clock`**，绝不用 `QTime`/`system_clock`（防用户改系统时间导致轮询错乱）。

> 渲染附加约束（ADR-22）：统一 `QTimer 30/60Hz` + 降采样 + `rpQueuedReplot`，禁数据到达即 `replot()`。  
> SBO 附加约束（ADR-23/FR-CTRL-07）：Armed 5s 超时/断链清锁；`DeviceSboGuard` 二维 key 防同址并发；UI 在 Armed 期间禁用其他操作。

*本文基于 ENS-HLD-000 V1.5、ENS-LLD-000 V1.3、ENS-LLD-100/200/400/500、ENS-DEV-GUIDE V4.0、ENS-(HLD|LLD|SIM)-SIM 及 ENS-HLD-ICD/DB/THREAD/UI/UX/BIZ/COMM/PROTO 汇总编制；所有路径、类名、Target、命名空间均严格沿用上游文档，作为 `apps/ens_app`（主程序）与 `apps/device_simulator`（测试台）双子工程物理代码结构的直接建目录依据。*
