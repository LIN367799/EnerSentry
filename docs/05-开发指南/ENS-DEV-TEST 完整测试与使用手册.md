# ENS-DEV-TEST · EnerSentry 完整测试与使用手册

> 适用版本：HEAD（S46 后：工具链入口、FC01/02/03/04 轮询与 Frameless 图标回归已收口）
> 基线：ctest **346/346** 全绿；双进程端到端验收脚本支持本机/团队构建目录自动选择；GUI 双应用冒烟 **PASS**
> 本文命令以 `D:\Study\Qt_host_application_Project\EnerSentry` 仓库根为执行目录。

---

## 0. 一分钟速查

| 我想做什么 | 命令（PowerShell，仓库根目录执行） |
|---|---|
| 全量跑一遍测试 | `ctest --test-dir build\vs2022-debug-local --output-on-failure`（本机，346 用例） |
| 端到端验收（一键） | `powershell -ExecutionPolicy Bypass -File tools\run_phase4_acceptance.ps1` |
| 长稳浸泡（60 分钟） | `powershell -ExecutionPolicy Bypass -File tools\run_soak_test.ps1 -Minutes 60` |
| 开 GUI 玩起来 | ① 先起 `bin\Debug\DeviceSimulator.exe` ② 再起 `bin\Debug\ens_app.exe --point-table data\sim_pointtable_sample.json`（登录 `admin` / `Admin@123`） |

---

## 1. 五层测试体系总览

| 层 | 内容 | 入口 | 耗时 | 判据 | 本次实测 |
|---|---|---|---|---|---|
| **L0 构建** | Ninja + MSVC v143 全目标 | `cmake --build build\vs2022-debug-local --parallel`（本机） | 增量秒级 / 全量分钟级 | `ninja: no work to do.` 或 0 error | ✅ 已最新 |
| **L1 单测/集成** | Catch2 346 用例（协议/通道/数据/业务/UI 纯函数） | `ctest --test-dir build\vs2022-debug-local --output-on-failure` | 约 90 s（本机实测） | 100% passed | ✅ 346/346 |
| **L2 端到端** | 双进程 CLI：模拟器 ⇄ 上位机全链路 | `tools\run_phase4_acceptance.ps1` | 12 s ~ 80 s（随场景） | 6 条断言全 OK | ✅ PASS |
| **L3 GUI 验收** | 双应用真实联调，7 个页面 + SBO + 断链 | 手动（本文 §5） | 10 ~ 15 min | 人工 checklist | ✅ 冒烟通过（offscreen） |
| **L4 浸泡** | 断链压力长跑，采样内存/CPU/句柄/入库 | `tools\run_soak_test.ps1` | 自定义（建议 ≥ 60 min） | 内存增长 < 20 MB 且入库速率不衰减 | ✅ 脚本可用（见 §6） |

---

## 2. L0 · 环境与构建

### 2.1 前置依赖

| 组件 | 版本 / 路径 | 用途 |
|---|---|---|
| Visual Studio 2022 | `D:\Program Files\VS2022`（v143 工具集） | MSVC 编译器 + LIB 环境 |
| Qt | `5.15.2 msvc2019_64`（`D:\HJL\qt\5.15.2\msvc2019_64`） | GUI / Network / SerialPort / Sql / Svg / PrintSupport |
| vcpkg | `D:\Tool\vcpkg`，triplet `x64-windows` | Catch2 / nlohmann_json / spdlog |
| CMake + Ninja | CMake ≥ 3.21 | 团队/CI 预设 `vs2022-debug`；本机预设 `vs2022-debug-local` |
| QCustomPlot | 仓库内 `3rdparty/` vendored | 曲线渲染 |

### 2.2 构建

> ⚠️ **必须用 VS 开发壳 PowerShell**。git bash 缺少 `LIB` 环境变量，会在链接期报 `LNK1104: 无法打开文件“ws2_32.lib”`。构建与 ctest 一律走 PowerShell。

```powershell
# ① 进入 VS 开发壳（每次新开 PowerShell 都要执行）
Import-Module "D:\Program Files\VS2022\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "D:\Program Files\VS2022" -Arch amd64 -SkipAutomaticLocation

# ② 配置（团队/CI：使用环境变量驱动的可移植 preset）
Set-Location D:\Study\Qt_host_application_Project\EnerSentry
cmake --preset vs2022-debug

# ③ 构建
cmake --build build/vs2022-debug
```

本机日常验证使用未入库的 `CMakeUserPresets.json`：

```powershell
Set-Location D:\Study\Qt_host_application_Project\EnerSentry
cmake --preset vs2022-debug-local
cmake --build build/vs2022-debug-local --parallel
ctest --test-dir build/vs2022-debug-local --output-on-failure
```

`build\vs2022-debug` 只用于环境变量 preset。若该目录来自旧坏缓存，例如 `CMAKE_CXX_COMPILER` / `CMAKE_MAKE_PROGRAM` 为空，或 `ctest -N` 显示 `Total Tests: 0`，不要继续用它做验证；删除后重配，或改用 `build\vs2022-debug-local`。

产物落在 `bin\Debug\`：`ens_app.exe`、`DeviceSimulator.exe`、`ens_tests.exe` + 各 `ens_*.dll` + Qt 运行期 DLL（已由 `ens_windeployqt` 自动部署）。

### 2.3 构建后自检

```powershell
cmake --build build/vs2022-debug-local --parallel     # 期望: ninja: no work to do.
Get-ChildItem bin\Debug\*.exe        # 期望: DeviceSimulator.exe / ens_app.exe / ens_tests.exe
```

---

## 3. L1 · 单元与集成测试（ctest 325）

```powershell
ctest --test-dir build\vs2022-debug-local --output-on-failure
ctest --test-dir build\vs2022-debug-local -R pointtable --output-on-failure
ctest --test-dir build\vs2022-debug-local -R alarm -j 8 --output-on-failure
```

测试源码分布：

| 目录 | 覆盖 | 典型文件 |
|---|---|---|
| `tests/unit/` | 协议帧 / CRC / MBAP / 点表 / 轮询 / 数据总线 / 告警引擎 / 黑匣子 / 认证 / SBO / 导出 / UI 纯函数 | `test_modbus_frame.cpp`、`test_alarm_engine.cpp`、`test_export_utils.cpp` |
| `tests/integration/` | 真环回通道 / 主循环端到端 / P1 收口 | `test_main_loop.cpp`、`test_modbus_tcp_loopback.cpp` |

本次实测尾部输出：

```
346/346 Test #346: poll_scheduler: enterProbingIfDue after 30s -> PROBING issued immediately ...   Passed
100% tests passed, 0 tests failed out of 346
Total Test time (real) = 90.00 sec
```

---

## 4. L2 · 双进程端到端验收

### 4.1 拓扑

```
DeviceSimulator.exe (Modbus TCP Server, 127.0.0.1:PORT)
        ▲  Modbus TCP 请求/响应
        │
ens_app.exe --cli (采集 → 点表 → L1 快照 → DataBus → 告警引擎 → 黑匣子/月库)
```

两个进程**必须同时在线**：ens_app 是主（Client），DeviceSimulator 是从（Server）。

### 4.2 一键验收（推荐）

```powershell
# 默认场景 overheat_fast（12 s）
powershell -ExecutionPolicy Bypass -File tools\run_phase4_acceptance.ps1

# 指定场景与时长
powershell -ExecutionPolicy Bypass -File tools\run_phase4_acceptance.ps1 -Scenario overheat_drill.json -RunSeconds 80
```

#### ⚠️ 场景时长铁律（本次实测踩坑）

`sim_report.json` 的 `result` 有三态：`PASS` / `FAIL` / `INCONCLUSIVE`。
**若 `-RunSeconds` 短于场景最后一步的时间点，场景被提前中止 → 报 `INCONCLUSIVE`，这不是功能缺陷。**

| 场景 | 最后一步 | 建议 `-RunSeconds` | 实测 |
|---|---|---|---|
| `overheat_fast.json`（默认） | RECOVER @ 5 s | **12**（默认） | ✅ PASS |
| `voltage_fault_drill.json` | RECOVER @ 40 s | **50** | ✅ PASS |
| `overheat_drill.json` | RECOVER @ 70 s | **80** | ✅ PASS |
| `random_linkloss_stress.json` | 300 s 窗口内 8 次断链 | 用 `run_soak_test.ps1` | 见 §6 |

脚本已内置**时长守卫**，短于需求时会提前告警：

```
  [!] WARN: -RunSeconds 20 < scenario needs ~75s (last step t=70000ms)
      expect sim_report result=INCONCLUSIVE; re-run with -RunSeconds 75
```

#### 6 条断言

| # | 断言 | 含义 |
|---|---|---|
| 1 | `sim_report result=PASS` | 场景所有步骤（含 RECOVER）全部触发完成 |
| 2 | `app link connected` | Modbus TCP 建链成功 |
| 3 | `app samples: N > 0` | 采集轮询产出样本 |
| 4 | monthly DB `history\YYYYMM\data_YYYYMM.db` | L2 历史库按月落盘 |
| 5 | `blackbox critical.swp` | Critical 告警触发黑匣子 mmap 落盘 |
| 6 | `alarm rules loaded` | 告警规则文件被加载 |

本次实测（`overheat_drill.json` @ 80 s）：

```
== Phase 4 acceptance ==
  sim: DeviceSimulator --cli 80s scenario=overheat_drill.json port=15031
  app: ens_app --cli 78s port=15031
  [OK] sim_report result=PASS
  [OK] app link connected
  [OK] app samples: 549
  [OK] monthly DB: ...\build\phase4_accept_out\db\history\202609\data_202609.db
  [OK] blackbox critical.swp
  [OK] alarm rules loaded
== PASS: Phase 4 dual-track acceptance assertions all passed ==
```

### 4.3 手动双进程（便于观察原始输出）

```bash
# 终端 1：模拟器（CLI，端口 15060，跑 30 s）
./bin/Debug/DeviceSimulator.exe --cli 30 --port 15060 \
    --pointtable build/vs2022-debug-local/test_data/sim_pointtable_sample.json

# 终端 2：上位机（CLI，跑 20 s 后自动退出）
./bin/Debug/ens_app.exe --cli \
    --point-table build/vs2022-debug-local/test_data/sim_pointtable_sample.json \
    --host 127.0.0.1 --port 15060 --run-seconds 20 \
    --alarm-rules build/vs2022-debug-local/test_data/alarm_rules_sample.json \
    --data-dir build/my_out/db --blackbox-dir build/my_out/bb
```

`ens_app` 正常输出形如：

```
[ENS] link connected
[ENS] pt=1 value=35.00
[ENS] pt=2 value=55.30
...
[ENS] link disconnected
[ENS] stopped (samples=56)
```

### 4.4 SBO（选择-确认-执行）一次性命令

`--cmd` 在启动 800 ms 后**注入一次**便清空，因此 `select` → `operate` 需在同一次 GUI 会话完成；CLI 下只能验证单步。

```bash
./bin/Debug/ens_app.exe --cli --point-table <pt.json> --host 127.0.0.1 --port 15052 \
    --run-seconds 6 --cmd "select:17:12288:1"
```

格式：`select:<slaveId>:<registerAddr>:<value>[:e]`（`:e` = 急停，armed 超时 3 s，常规 5 s）；另有 `operate` / `cancel`。

实测输出：

```
[ENS] link connected
[ENS] SBO select submitted=1 seq=sbo-0
[ENS] stopped (samples=56)
```

样例点表中**可写点仅 2 个线圈**（其余为只读寄存器）：

| 点名 | 从站 | 寄存器地址 | 类型 |
|---|---|---|---|
| `PCS-01_ExhaustCtrl` | 17 | 12288 | Coil / Bool |
| `PCS-01_LiquidCtrl` | 17 | 16384 | Coil / Bool |

---

## 5. L3 · GUI 完整使用与人工验收

### 5.1 启动顺序（重要）

模拟器是服务端，**必须先起**，否则上位机启动即报「通信内核启动失败」。

```powershell
# ① 模拟器（GUI，默认监听 127.0.0.1:5020）
.\bin\Debug\DeviceSimulator.exe --pointtable data\sim_pointtable_sample.json

# ② 上位机（--point-table 为必填项，缺失时打印 usage 并以退出码 2 结束）
.\bin\Debug\ens_app.exe --point-table data\sim_pointtable_sample.json --host 127.0.0.1 --port 5020
```

> 设计哲学：上位机**禁止**无点表启动，防止误连真实设备。VS 调试请在「调试 → 命令参数」填入 `--point-table data/sim_pointtable_full.json`。

### 5.2 登录

| 账号 | 密码 | 角色 | 权限 |
|---|---|---|---|
| `admin` | `Admin@123` | Admin | 全量（含用户管理、SBO、参数配置、导出/备份） |
| `operator` | `Operator@123` | Operator | **只读**：SBO 控制与参数配置被禁用 |

> `config\users.json` 不存在时回退内置账号，启动日志可见
> `AuthManager: users.json 缺失或为空，回退内置默认用户（2 个）`。
> 用户管理页保存后会生成该文件（随机 salt + SHA-256）。

### 5.3 七个页面逐个验收

| 页面 | 你能看到什么 | 验收动作 | 期望 |
|---|---|---|---|
| **总览 Overview** | SOC 仪表盘、簇温热力条、关键点卡片 | 观察 5 ~ 10 s | 数值随采集刷新，SOC 有变化 |
| **实时曲线 Trend** | QCustomPlot 多通道滚动曲线 | 鼠标移动看 hover 十字线；勾选/取消通道 | 曲线持续左移，hover 显示时间+数值 |
| **告警中心 Alarm** | 活动告警列表 + 历史查询 | 在模拟器注入过温，等约 3 s | 出现 Critical 行；弹窗 + 蜂鸣 + 任务栏闪烁 |
| **历史趋势 History** | 按月库查询的历史曲线 | 选点 + 时间范围 → 查询 | 需以 `--data-dir` 启动才有数据，否则返回空 |
| **SBO 控制** | 选择 → 执行/取消，armed 倒计时 | `Select` → 5 s 内点 `Operate` | 状态 `Idle→Armed→Done`；超时自动回 Idle |
| **诊断 Diag** | 通道统计、链路状态、点表/规则信息 | 停掉模拟器 | 状态栏转红 `disconnected`；重启模拟器后转绿 |
| **配置 Config** | 点表/规则路径、导出配置、备份历史库、用户管理 | 点「导出配置」 | 需带 `--point-table` 路径；未带 `--data-dir` 时备份按钮禁用 |

### 5.4 关键人工验收项（切片 22 定义）

1. 起模拟器 → 起上位机 → 登录 `admin` → 实时曲线页 8 通道滚动。
2. SBO 页：`Select` → `Armed`（5 s 倒计时）→ `Operate` 成功。
3. 停掉模拟器 → 状态栏红 `disconnected`；重启模拟器 → 自动重连转绿。
4. 任务管理器观察：实时曲线页 CPU **< 15%**（OpenGL 自动探测生效）。

### 5.5 offscreen 自动化冒烟（CI/无人值守可用）

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
# 起两进程后断言：双进程存活 + 5020 处于 LISTEN
```

本次实测：**sim alive True / app alive True / 5020 LISTEN True，无崩溃**。

> 说明：offscreen 下 Qt 会刷
> `QFontDatabase: Cannot find font directory ...\lib\fonts` 与
> `This plugin does not support propagateSizeHints()`，属**平台插件噪音**，真实桌面运行不出现，可忽略。

---

## 6. L4 · 浸泡 / 长稳测试

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_soak_test.ps1 -Minutes 60
powershell -ExecutionPolicy Bypass -File tools\run_soak_test.ps1 -Minutes 3 -SampleSec 15   # 快速自检
```

- 场景：`random_linkloss_stress.json`（300 s 窗口内 8 次断链注入 + 自动恢复）。
- 每 `SampleSec` 秒采样一行到 `build\soak_out\soak.csv`：
  `time_s, simMemKB, appMemKB, appCpuPct, appHandles, sampleLines, dbSizeKB`。
- 判定：
  1. **内存收敛**：末 10% 均值 − 首 10% 均值 **< 20 MB**；
  2. **采集速率不衰减**：末段速率 ≥ 首段 **60%**；
  3. 跑到终点的进程仍存活（无崩溃/无挂死）。
- 采样行数需 **≥ 6** 才进入判定，否则报 `insufficient samples in CSV`，短时自检请把 `SampleSec` 调小（如 `-Minutes 3 -SampleSec 15` → 约 11 行）。

判定（切片 44b 已修正为**可失败**的语义）：

| 判据 | 规则 |
|---|---|
| 全程未采数 | `sampleLines` 末值为 0 → FAIL（`no samples ingested at all`） |
| 启动即哑火 | 首窗增量 ≤ 0 → FAIL（`no ingestion in the first window`） |
| 采集速率衰减 | 末窗增量 < 首窗增量 × 60% → FAIL |
| 内存泄漏 | 末窗内存均值 − 首窗内存均值 > 20 MB → FAIL |
| 挂死 | 超时后进程仍存活 → FAIL 并强杀 |

> ⚠️ **两列读法（易踩坑）**
> - `sampleLines`：**阶梯式跳变，不是实时值**。重定向到文件的 stdout 是全缓冲，约每 4 KB（≈169 条样本行）才 flush 一次，所以你会看到 `0 → 169 → 169 → 346 …` 的台阶。判断"有没有在采数"要看**末值是否在涨**，不能看单点。
> - `dbSizeKB`：**不可作为入库指标**。SQLite 页复用 + WAL 下，实测 3 分钟内月库文件恒定 4 KB 不动，但数据确实在写。

3 分钟自检实测基线：

```
  t=15s appMem=50MB cpu=45.8% handles=445 samples=0    db=4KB
  t=90s appMem=51MB cpu=0.9%  handles=439 samples=523  db=4KB
  t=165s appMem=51MB cpu=0.5% handles=431 samples=1054 db=4KB
  mem growth (last2 rows - first2 rows): 0.7 MB
== PASS: soak test stable ==
```

---

## 7. 数据工件与目录约定

| 路径 | 内容 |
|---|---|
| `data\sim_pointtable_sample.json` | 样例点表（**43 点 / 8 从站**：Rack-01/02、PCS-01/02、Meter、Liquid、Fire） |
| `data\sim_pointtable_full.json` | 全量点表（8 MB，由 `docs/04-测试台/tools/ptgen.py --full` 生成） |
| `data\alarm_rules_sample.json` | 告警规则样例（Rack-01/02 MaxTemp，Critical，阈值 60/55，3 s 延时，60 s 抑制窗） |
| `data\scenarios\*.json` | 4 套演练场景（见 §4.2 表） |
| `build\vs2022-debug-local\test_data\` | 本机 local preset 的测试数据副本（脚本默认优先读这里） |
| `build\vs2022-debug\test_data\` | 团队/CI 环境变量 preset 的测试数据副本（无 local 目录时脚本回退读这里） |
| `<--data-dir>\history\YYYYMM\data_YYYYMM.db` | L2 历史月库 |
| `<--data-dir>\alarm\YYYYMM\alarm_YYYYMM.db` | 告警月库 |
| `<--blackbox-dir>\critical.swp` | Critical 告警黑匣子交换文件 |

> **铁律**：运行期数据 JSON 一律放仓根 `data/`，禁止回迁 `docs/04-测试台/`（该目录只留文档与 `ptgen.py`）。

---

## 8. 命令行参数全表

### 8.1 `ens_app.exe`

| 参数 | 默认 | 说明 |
|---|---|---|
| `--cli` | 关 | 无窗口 headless 模式 |
| `--point-table <path>` | **必填** | 点表 JSON；缺失 → `usage: --point-table <json> is required`，退出码 2 |
| `--host` | `127.0.0.1` | 模拟器/设备 IP |
| `--port` | `5020` | Modbus TCP 端口 |
| `--poll-ms` | `100` | 轮询间隔 |
| `--run-seconds` | `0` | 自动退出秒数（0 = 常驻） |
| `--alarm-rules <path>` | 空 | 告警规则 JSON（空 = 不启用规则） |
| `--data-dir <dir>` | 空 | 历史/告警月库根（空 = 不落盘，历史查询返空） |
| `--blackbox-dir <dir>` | 空 | 黑匣子目录（空 = 仅计数不落盘） |
| `--cmd <cmd>` | 空 | SBO 一次性命令：`select:s:r:v[:e]` / `operate` / `cancel` |
| `--users <path>` | `config/users.json` | 用户表（缺失回退内置 2 账号） |

### 8.2 `DeviceSimulator.exe`

| 参数 | 默认 | 说明 |
|---|---|---|
| `--cli` | 关 | 无窗口模式（联调脚本用） |
| `[runSec]`（位置参数） | `5` | 仅 CLI 模式有效 |
| `--scenario <path>` | 空 | 演练场景 JSON |
| `--export-dir <dir>` | 空 | 导出 `sim_report.json` / `sim_events.jsonl` |
| `--pointtable <path>` | `data/sim_pointtable_sample.json` | 点表（与上位机必须同源） |
| `--port <n>` | CLI:0（OS 分配）/ GUI:5020 | TCP 监听端口 |

> RTU 默认 **关闭**（`rtu.enabled=false`），开启需在 GUI「运行配置」勾选并备好 com0com 虚拟串口对。

---

## 9. 故障排查表

| 症状 | 根因 | 处理 |
|---|---|---|
| `LNK1104: 无法打开文件“ws2_32.lib”` | 在 git bash 里构建，缺 `LIB` 环境变量 | 一律用 VS 开发壳 PowerShell |
| `cmake -B` 后 vcpkg 依赖找不到 | 未走预设，丢了 `CMAKE_TOOLCHAIN_FILE` | 团队/CI 用 `cmake --preset vs2022-debug`；本机用 `cmake --preset vs2022-debug-local` |
| `build\vs2022-debug` 构建报 `no such file or directory` 或 `ctest -N` 为 0 | 旧坏缓存未配置出有效 Ninja/测试清单 | 删除 `build\vs2022-debug` 后按环境变量重配，或使用 `build\vs2022-debug-local` |
| `[ENS] usage: --point-table <json> is required`（退出码 2） | 生产设计：禁止无点表启动 | 补 `--point-table`；VS 调试填「命令参数」 |
| `sim_report result=INCONCLUSIVE` | `-RunSeconds` < 场景最后一步时刻 | 按 §4.2 表加长（脚本会打印 WARN 与建议值） |
| 上位机弹「通信内核启动失败」 | 模拟器未先起 / 端口不一致 / 点表不同源 | 先起模拟器；两边端口与点表保持一致 |
| 历史趋势页查询为空 | 未带 `--data-dir`（未启用持久化） | 加 `--data-dir <dir>` 重启 |
| 备份/导出按钮灰色 | 缺 `--point-table` 或 `--data-dir` | 按 §5.3 配置页说明补齐 |
| 改 Q_OBJECT/头文件后莫名 SIGSEGV 或 getter 读漂移值 | stale moc/消费方 obj | `ninja -t clean` 后重编**全部消费目标**（含 tests） |
| 链接期 `LNK1168` exe 被占用 | 上一轮进程残留 | `tasklist \| grep -i <exe>` 找 PID → `MSYS_NO_PATHCONV=1 taskkill /F /PID <pid>`，再 `tasklist` 复查 |
| 验收脚本报 `Wait-Process : 进程(NNN)已退出` | `Wait-Process` 对已退出进程抛异常，`-ErrorAction` 抑制不住，叠加 `Stop` 中断脚本 | **已修**（切片 44b）：改为轮询 `HasExited` 的 `Wait-Proc` |
| 浸泡脚本第一采样点即崩（`值不能为 null`） | 重定向 stdout 全缓冲 → 文件 0 字节 → `[regex]::Matches($null,…)` 抛空引用 | **已修**（切片 44b）：加空值守卫；`sampleLines` 降级为参考列 |
| 脚本改完报 `UnexpectedToken '}'` | **PS 5.1 把无 BOM 的 .ps1 按 ANSI/GBK 解码**，中文注释炸成非法 token | `tools\*.ps1` 必须**保持纯 ASCII**（文件头已写 ENCODING CONTRACT 注释） |
| offscreen 下刷 `QFontDatabase: Cannot find font directory` | 平台插件无字体目录 | 仅 offscreen 噪音，桌面运行不出现 |

---

## 10. 历史验证记录（切片 44b）

| 项 | 结果 |
|---|---|
| `cmake --build` | `ninja: no work to do.`（产物与 HEAD 一致） |
| `ctest --preset debug` | **325/325 passed**，533 s（历史记录；当前本机入口见 §3，使用 `build\vs2022-debug-local`，346 tests） |
| 双进程验收 · overheat_fast @12 s | **PASS**（6/6 断言） |
| 双进程验收 · voltage_fault_drill @50 s | **PASS**（346 samples） |
| 双进程验收 · overheat_drill @80 s | **PASS**（549 samples） |
| 时长守卫（overheat_drill @20 s） | 正确告警 + 判 `INCONCLUSIVE` |
| SBO `--cmd select:17:12288:1` | `submitted=1 seq=sbo-0` |
| GUI offscreen 双进程冒烟 | sim/app 存活，5020 LISTEN，无崩溃 |
| 浸泡脚本 3 min 自检 | **PASS**（内存增长 0.7 MB，1054 样本，速率无衰减） |
| 修复文件 | `tools\run_phase4_acceptance.ps1`、`tools\run_soak_test.ps1`（均保持纯 ASCII） |

### 10.1 本轮修掉的 3 个测试工具缺陷

| # | 缺陷 | 根因 | 修复 |
|---|---|---|---|
| 1 | 验收脚本随机崩溃 `Wait-Process : 进程已退出` | `Wait-Process` 对已退出进程抛异常，`-ErrorAction SilentlyContinue` 抑制不住，叠加 `$ErrorActionPreference='Stop'` 中断脚本；且 `-Timeout 60` 硬上限使长场景（80 s）必然超时 | 改为轮询 `HasExited` 的 `Wait-Proc`，超时改为 `simSec+30` |
| 2 | 浸泡脚本首采样点崩溃 `值不能为 null` | 重定向 stdout 全缓冲 → 采样时文件 0 字节 → `Get-Content -Raw` 返 `$null` → `[regex]::Matches($null,…)` 抛 `ArgumentNullException` | 加空值守卫 + 注释说明缓冲语义 |
| 3 | 浸泡「采集速率不衰减」断言**永远不可能失败** | 窗口取 10%×11 行 = 1 行，而首行 `sampleLines` 恒为 0（缓冲未 flush）→ `firstRate=0` → `lastRate < 0*0.6` 恒假 | 窗口改为 20% 且至少 2 行；新增「全程未采数 / 首窗哑火」两条硬失败 |

> 附带约束：`tools\*.ps1` **必须保持纯 ASCII**。PS 5.1 对无 BOM 的 `.ps1` 按 ANSI/GBK 解码，中文注释会被解析成非法 token，报错是极具误导性的 `UnexpectedToken '}'`。两个脚本文件头已写入 ENCODING CONTRACT 注释。
