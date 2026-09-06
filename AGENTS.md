# EnerSentry 项目开发规范

本文档是 EnerSentry 项目的 Codex/开发代理级规范。后续开发必须遵守本文件以及用户在当前任务中提出的更高优先级要求。

## 1. 项目简介

EnerSentry 是一个基于 Qt Widgets 的储能/工业设备监控主站项目，包含：

- Modbus TCP 主站采集。
- 点表驱动的轮询、数据解码和采样发布。
- 实时总览、实时曲线、历史趋势和通信诊断。
- 告警规则、迟滞、延时、确认、恢复和告警历史。
- Critical 告警黑匣子快照。
- SQLite 历史数据和告警记录。
- 用户认证、角色权限、审计和会话锁定。
- Select-Before-Operate（SBO）控制。
- DeviceSimulator 设备模拟器、故障注入和场景演练。

项目当前的主运行链路以 TCP 为主。SerialChannel、BusinessStateMachine 和部分多粒度能力已经存在，但并不都已经接入主程序运行流程。

## 2. 已确认技术栈

- 语言：C++。
- C++ 标准：C++17。
- UI：Qt Widgets、`.ui`、C++，不是 QML。
- Qt：Qt 5.15.2，Windows 构建使用 `msvc2019_64` 套件。
- 构建系统：CMake，最低版本要求 3.21。
- 构建生成器：Ninja。
- 编译器基线：Visual Studio 2022、MSVC v143。
- 依赖管理：vcpkg，triplet 为 `x64-windows`。
- JSON：nlohmann-json。
- 日志/格式化依赖：spdlog、fmt。
- 测试：Catch2 + CTest。
- 图表：仓库内 vendored 的 QCustomPlot。
- 平台相关 mmap：Windows 和 POSIX 分离实现。

Qt、编译器和第三方库必须使用相同 ABI 体系。不得使用 MinGW 的 g++/ld 去链接 `Qt 5.15.2 msvc2019_64` 的库。

## 3. 构建方法

推荐先加载 VS2022 x64 开发环境，再使用 Ninja 配置。项目预设位于 `CMakePresets.json`，不包含任何开发者本机绝对路径；配置通过环境变量提供 Qt、vcpkg、Ninja 和 MSVC 编译器位置。

在 VS2022 x64 Developer PowerShell 中设置当前机器的工具路径（路径按本机安装位置调整）：

```powershell
$env:QT_ROOT = "<Qt>/5.15.2/msvc2019_64"
$env:VCPKG_ROOT = "<vcpkg>"
$env:NINJA_EXECUTABLE = "<ninja>/ninja.exe"
$env:MSVC_CXX_COMPILER = "<VS2022>/VC/Tools/MSVC/<version>/bin/Hostx64/x64/cl.exe"
```

正式配置命令：

```powershell
cmake --preset vs2022-debug
```

编译：

```powershell
cmake --build build/vs2022-debug --parallel
```

Release 使用 `cmake --preset vs2022-release` 和 `cmake --build build/vs2022-release --parallel`。配置阶段会拒绝 MinGW/g++、非 cl.exe 编译器、旧版 MSVC 以及非 `msvc2019_64`/非 5.15.2 Qt；不会等到链接阶段才报 ABI 错误。

默认构建目标包括：

- `ens_app`：主程序。
- `DeviceSimulator`：设备模拟器。
- `ens_tests`：测试程序。

构建输出通常位于 `bin/Debug` 或 `bin/Release`。Qt DLL、平台插件和 SQL 驱动由 `EnsDeploy.cmake` 负责部署。

## 4. 运行方法

### 主程序 CLI

主程序当前要求提供点表路径：

```powershell
bin/Debug/ens_app.exe --cli `
  --point-table data/sim_pointtable_sample.json `
  --host 127.0.0.1 `
  --port 5020 `
  --poll-ms 100 `
  --run-seconds 10
```

### 主程序 GUI

```powershell
bin/Debug/ens_app.exe --point-table data/sim_pointtable_sample.json
```

GUI 启动会先加载用户表、主题并显示登录窗口，然后创建 `EnerSentryApp` 和 `MainWindow`。

### 设备模拟器

先启动模拟器，再启动主程序：

```powershell
bin/Debug/DeviceSimulator.exe --cli `
  --pointtable data/sim_pointtable_sample.json `
  --port 5020 10
```

场景演练可使用 `data/scenarios/` 下的 JSON 文件，并根据 `main_gui.cpp` 支持的参数传入 scenario/export 目录。

TODO：GUI 默认点表路径、完整部署启动器和所有运行参数仍需以产品部署约定为准。

## 5. 测试方法

配置并编译后，可使用：

```powershell
ctest --test-dir build/vs2022-debug --output-on-failure
```

也可以直接运行：

```powershell
bin/Debug/ens_tests.exe
```

测试包含协议、通信、数据层、业务层、UI、模拟器和端到端集成测试。测试可能创建临时 SQLite、场景输出或其他运行期数据，运行前应确认工作区状态，运行后不要把生成物当作源代码提交。

当前规范不假设测试全部通过。修改后必须报告实际执行的测试命令和结果。

## 6. 目录结构

```text
apps/ens_app/                 主站程序
  src/app/                    入口和总编排
  src/channel/                TCP/串口通道
  src/protocol/               Modbus、点表、轮询
  src/datahub/                L1/L2 数据、SQLite、黑匣子
  src/business/               告警、认证、SBO、业务状态机
  src/ui/                     Qt Widgets 界面
  config/                     用户配置
apps/device_simulator/        设备模拟器
data/                         点表、告警规则、场景脚本
resources/                    qrc、SVG 图标和 QSS
3rdparty/                     QCustomPlot
tests/                        Catch2 单元和集成测试
cmake/                        CMake 辅助模块
docs/                         需求、设计和开发文档
```

## 7. 核心模块和类

- `EnerSentryApp`：主程序对象编排、线程、信号槽和生命周期管理。
- `TcpChannel` / `SerialChannel`：通信通道实现。当前主程序实际使用 TCP。
- `ModbusEngine`：Modbus 请求发送、帧解析和事务路由。
- `PollScheduler`：轮询调度、优先级和链路健康状态。
- `PointTable`：JSON 点表和数据类型定义。
- `L1SnapshotStore` / `RingBuffer`：高频内存采样。
- `DataBus`：采样观察者广播。
- `DownSampler` / `L2HistoryStore`：历史降采样和批量持久化。
- `SQLiteDataAccess`：历史库和告警库访问。
- `BlackBoxManager` / `CriticalSwapFile`：Critical 告警快照和 mmap 存储。
- `AlarmEngine`：告警状态计算和告警事件生成。
- `AlarmRecordStore`：告警事件持久化。
- `AuthManager`：认证、角色、权限、审计和会话。
- `SboStateMachine` / `DeviceSboGuard`：SBO 控制和设备级互斥。
- `MainWindow`：主界面、页面切换、权限过滤和状态栏。
- `SimulatorEngine` / `FaultInjector` / `ScenarioScript`：模拟器运行、故障和场景。

## 8. C++ 代码规范

- 使用 C++17；不得无理由引入更高标准特性。
- 优先使用 RAII、智能指针、明确的所有权和 const-correctness。
- 资源、线程、文件、数据库连接和 QObject 生命周期必须有明确的拥有者。
- 避免无关重构、批量格式化和大范围命名变更。
- 新增功能应优先复用现有接口和数据结构，不重复创建平行抽象。
- 读写边界、线程边界和错误处理必须清晰。
- 只在复杂算法、线程模型或兼容性约束处添加简短注释。
- 项目已有中文注释时保持文件编码一致；不要引入不可见字符或混合编码。

## 9. Qt 开发规范

- 使用 Qt 5.15.2 API；不要未经确认使用 Qt 6 专有 API。
- UI 优先使用现有 `.ui` 和 QWidget 结构。
- 页面通过 `UiDeps` 注入业务依赖；UI 层不得直接包含 `EnerSentryApp.h` 或创建协议/数据库核心对象。
- UI 不得在高频采样回调中执行阻塞数据库、文件或复杂绘图操作。
- 资源图标和 QSS 必须通过 `resources.qrc` 注册后使用 `:/icons/...`、`:/qss/...` 路径。
- QCustomPlot 相关对象必须在 GUI 线程使用。
- High DPI、无边框窗口和 OpenGL 回退逻辑修改时必须补充对应 UI 测试或手工验证。

## 10. QObject 生命周期规范

- QObject 有 parent 时，不得依赖 `moveToThread()` 移动它；需要跨线程移动的对象通常必须以 `nullptr` parent 创建。
- parent-child 关系应表达真实所有权，不要使用悬空裸指针作为拥有者。
- QObject 所属线程必须与其定时器、socket、数据库连接和槽执行线程一致。
- 停止顺序必须先停止定时器/IO，再停止线程，最后释放对象。
- `deleteLater()` 只能在对象所属线程事件循环仍可处理时使用。
- 破坏性析构时要确认所有 queued signal 不会再访问已释放对象。

## 11. signal/slot 规则

- 优先使用 Qt 新式函数指针连接，不使用无法检查签名的旧字符串连接。
- 跨线程信号必须确认参数已注册 metatype，并明确使用 queued/direct 的原因。
- 高频采样信号的接收者必须轻量，UI 更新应通过主线程定时器或队列合并。
- 信号只表达状态变化或事件，不通过信号隐藏同步阻塞操作。
- 连接对象生命周期不明确时使用 context object，避免 lambda 捕获悬空对象。
- 修改 signal/slot 参数时同时检查所有 connect、metatype 注册和测试。

## 12. 多线程注意事项

- 主线程负责 QApplication、窗口和 UI。
- `EnerSentryApp` 的 Modbus engine、scheduler 和 PollDriver 使用 IO worker 线程。
- DataBus 当前是同步广播；订阅者不得在回调中阻塞或执行重操作。
- QTcpSocket/QSerialPort 只能在所属线程访问。
- SQLite 连接不能在创建它的线程之外使用；每个线程的数据库连接必须有清晰策略。
- 共享状态必须使用已有 mutex/atomic 约定，不要在没有分析调用线程的情况下添加锁或改变连接类型。
- 任何线程模型变更必须检查 stop、析构、queued signal 和异常路径。

## 13. UI 与业务逻辑分离

- UI 只负责展示、用户输入、页面状态和调用注入的接口/回调。
- 告警规则计算、采集调度、协议组帧、数据库 schema 和权限判断不得写入 UI 类。
- 主程序接线集中在 `EnerSentryApp`；新增服务应通过依赖注入暴露给 UI。
- UI 不得直接操作底层 `TcpChannel`、`ModbusEngine` 或 `SQLiteDataAccess` 的内部实现。

## 14. CMake 修改规则

- 先确认目标归属，再修改对应的 `CMakeLists.txt`。
- 新增 QObject、`.ui` 或 `.qrc` 文件时确认 AUTOMOC/AUTOUIC/AUTORCC 的 source 列表和资源路径。
- 不要把本机临时绝对路径写入通用工程配置；当前测试 CMake 中已有本机路径，修改时需评估可移植性。
- 不要改变 Qt 编译器套件、vcpkg triplet 或 target 类型而不做完整配置和链接验证。
- 保持依赖方向：`ens_app -> ens_ui -> ens_business -> ens_datahub -> ens_protocol -> ens_channel`。
- 测试专用 source 可以加入 `tests/CMakeLists.txt`，但必须说明为何需要直接编译生产或模拟器源文件。

## 15. 不要随意修改的内容

- `apps/ens_app/src/app/EnerSentryApp.*`：全局接线和生命周期核心。
- `apps/ens_app/src/protocol/`：协议、地址、数据类型和轮询契约。
- `apps/ens_app/src/datahub/`：采样内存、数据库和黑匣子数据契约。
- `apps/ens_app/src/business/AlarmEngine.*`：告警安全行为。
- `apps/ens_app/src/business/AuthManager.*`：认证和权限边界。
- `apps/ens_app/src/business/SboStateMachine.*`：控制安全状态机。
- `resources/resources.qrc`：两个应用和测试共享的资源注册。
- `data/*.json` 和 `data/scenarios/*.json`：联调、测试和演示输入。
- `3rdparty/qcustomplot/`：第三方源码，除非有明确升级或兼容性任务。
- `tests/`：测试是行为契约的一部分，不得为了让构建通过而删除或弱化测试。

工作区已有未提交修改时，必须先读取并理解相关 diff，不能覆盖、回退或清理用户修改。

## 16. 当前待确认事项

以下事项来自项目分析，不代表可以在本次任务中自动修复：

- 主程序 GUI 当前要求 `--point-table`，默认启动行为待确认。
- PollDriver 对非 FC03 响应的支持待确认。
- 模拟器专用 Float32 解码与真实设备协议兼容性待确认。
- 告警风暴定时器的启动生命周期待确认。
- SBO Modbus 写响应是否完整连接到 `onDeviceAck()` 待确认。
- `BusinessStateMachine` 是否应接入正式运行时待确认。
- L2 多粒度持久化和黑匣子启动恢复流程待确认。
- `placeholder_view` 是否仍被外部流程依赖待确认。
- 生产密码算法和明文兼容策略待确认。

代理不得把这些 TODO 当作授权，必须在用户明确要求后再修改。

## 17. 修改后的验证要求

任何代码修改至少需要：

1. 检查 `git diff`，确认没有无关文件变化。
2. 使用 MSVC + Qt `msvc2019_64` 重新配置或确认构建目录工具链正确。
3. 编译受影响目标；涉及公共接口、线程、CMake 或资源时执行完整构建。
4. 执行与修改范围匹配的单元/集成测试。
5. 涉及通信时使用 DeviceSimulator 或 loopback 验证。
6. 涉及 UI 时验证 Qt 平台插件、资源、High DPI 和基本窗口行为。
7. 报告未执行的测试、环境限制和仍存在的失败。

不得通过删除测试、关闭警告、跳过链接目标或修改用户配置来掩盖失败。

## 18. Git 提交规范

项目现有资料未定义正式提交格式。当前采用以下待确认的保守约定：

- 提交应只包含一个清晰主题。
- 推荐使用 Conventional Commits 风格，例如：`fix(protocol): ...`、`feat(ui): ...`、`test(datahub): ...`、`build(cmake): ...`。
- 提交标题使用英文短句，正文说明行为变化、验证命令和已知限制。
- 不提交 `build/`、`bin/`、`.vs/`、`outputs/`、临时数据库和运行日志。
- 不覆盖其他开发者的未提交修改。
- 用户未明确要求时，不自动创建提交、不自动推送、不自动修改分支。

TODO：正式团队提交格式、分支命名和评审流程需由项目维护者确认后补充。
