# ENS-SIM-IMP 《设备模拟与故障注入程序 实现规格补充》

> **文档编号**：ENS-SIM-IMP ｜ **版本**：V1.0 ｜ **配套**：ENS-HLD-SIM（概要设计）、《ENS-LLD-SIM》（详细设计）
> **读者**：实现开发人员 ｜ **编写日期**：2026-08-14
> **定位**：填补 HLD-SIM / LLD-SIM 与"可编码实现"之间的**具体工件缺口**。本册**不重复**两份上游文档已确定的设计结论（形态、寄存器映射、五类故障、线程模型、RCU、单测策略），只补"设计→代码"之间必须落地的**规格、常数、结构、数据样例与验收判据**。

---

## 0. 目的与范围

HLD-SIM / LLD-SIM 已完成"设计"，但开发人员直接动手时仍会被以下问题阻塞：

| 缺口 | 内容 | 本册章节 |
|------|------|----------|
| #1 | ICD 点表真实数据文件（枚举好的点表，而非仅寄存器 MAP） | §3 |
| #2 | 公共类型/协议库契约 `ens::core`（放哪、含什么） | §2 |
| #3 | 物理演化模型的具体常数表（OCV 曲线、热系数、默认初值） | §4 |
| #4 | CMake 工程脚手架（两目标结构与源文件清单） | §5 |
| #5 | 2–3 个完整可加载场景脚本 JSON | §7 |
| #6 | 线程库决策（standalone 用纯 `std::thread` 还是 Qt） | §9 |
| #7 | 观测日志 JSON schema（NFR-TEST-02） | §8 |

**本册不要求实现主程序 `ens::app`**。独立模拟器 `DeviceSimulator` 可单独编译运行，被任意 Modbus 客户端（含未来的主程序 `TcpChannel`）连接。LLD-SIM §7.4 描述的"驱动主程序回归验证"在主程序代码就绪后再启用。

---

## 1. 代码组织与目录结构（建议）

单仓库 `EnerSentry` 采用**顶层 CMake + 两个应用子项目**：`apps/ens_app`（上位机）与 `apps/device_simulator`（测试台 GUI）。测试台的模拟 / 故障逻辑作为 `device_simulator` 工程**内部的纯 C++17 模块**（`src/sim/`）组织，**不单独编译为共享库、不提供命令行 / headless 入口**——这正是 FR-SIM-08 的落地形态（对应 HLD-SIM §2.2 / §2.5）。两工程经 Modbus TCP（5020，承载 BMS/PCS/电表）+ Modbus RTU 虚拟串口（承载液冷/消防辅机）**双链路同时对话**，完美镜像主程序双栈部署，主程序零改动接入（FR-SIM-09）。

```
EnerSentry/                              # 单仓库（git）
├── CMakeLists.txt                       # 顶层，add_subdirectory 聚两子目录
├── apps/
│   ├── ens_app/                         # 上位机主程序（被测系统）
│   │   ├── CMakeLists.txt               # 定义 ens::app EXECUTABLE
│   │   └── ...                          # 主程序源码（不在本册范围）
│   └── device_simulator/                # 测试台应用
│       ├── CMakeLists.txt               # 定义 DeviceSimulator（Qt GUI EXECUTABLE，§5）
│       ├── src/
│       │   ├── sim/                     # 引擎模块（FR-SIM-08，纯 C++17，零 Qt）
│       │   │   ├── simulator_engine.*    # SimulatorEngine
│       │   │   ├── point_generator.*     # PointGenerator
│       │   │   ├── modbus_slave.*        # ModbusSlaveEmulator
│       │   │   ├── fault_injector.*      # FaultInjector
│       │   │   ├── scenario_script.*     # ScenarioScript
│       │   │   ├── register_bank.*       # RegisterBank（RCU 快照）
│       │   │   └── sim_config.*          # SimConfig 加载
│       │   ├── core/                     # 公共契约（§2，纯 C++17）
│       │   │   ├── point_table.*         # PointTableEntry
│       │   │   ├── crc16.*               # CRC-16/MODBUS
│       │   │   └── mbap.*                # MBAP 解析
│       │   ├── gui/                      # Qt 5.15 Widgets 前端（FR-SIM-10）
│       │   │   ├── main_window.*         # SimulatorMainWindow
│       │   │   ├── register_view.*
│       │   │   ├── fault_panel.*
│       │   │   ├── scenario_runner.*
│       │   │   └── log_view.*
│       │   └── main_gui.cpp              # GUI 入口（QApplication）
│       ├── tools/ptgen.py               # 点表全量生成器（§3 附录 A）
│       ├── scenarios/                   # 场景脚本样例（§7）
│       │   ├── overheat_drill.json
│       │   ├── random_linkloss_stress.json
│       │   └── voltage_fault_drill.json
│       └── data/
│           └── sim_pointtable_sample.json  # 代表性点表样例（§3）
```

**关键边界**：`device_simulator/src/sim` 与 `src/core` 为**纯 C++17**（原生 socket + `std::thread`），**不依赖 Qt**（无信号槽），便于隔离单测；Qt GUI（`src/gui/`、`main_gui.cpp`）单独链接 QtWidgets，作为薄前端消费 RCU 快照（NFR-TEST-04）。`apps/ens_app` 与 `apps/device_simulator` **互不依赖**——主程序编译时根本不需要测试台源码存在，确保主程序零耦合（NFR-TEST-03）。

---

## 2. 公共契约库 `ens::core`（自包含，零主程序依赖）— 缺口#2

### 2.1 决策

主程序（ICD §7.1 / HLD-COMM）已定义 `PointTableEntry`、CRC、MBAP 等类型。但**独立模拟器不应与主线构建耦合**：

- `DeviceSimulator` 自带一份**自包含的** `ens::core`（类型与算法与主程序**逐字节一致**，但物理拷贝，不共享编译单元）。
- 理由：模拟器常先于主程序存在、且需在 CI 容器里独立编译；耦合主线会拖垮二者。
- 将来若主线抽出公共 `ens::core`，可统一引用，模拟器仅需改 include 路径，**对外行为不变**。

### 2.2 头文件与类型（与 LLD-SIM §4.1 严格一致）

```cpp
// ens/core/modbus_types.h
namespace ens::core {
    enum class RegisterType : uint8_t { Coil = 0, DiscreteInput = 1, HoldingRegister = 2, InputRegister = 3 };
    enum class DataType    : uint8_t { Bool = 0, Int16 = 1, Uint16 = 2, Int32 = 3, Float32 = 4, Float64 = 5 };
    enum class ByteOrder   : uint8_t { ABCD = 0, BADC = 1, CDAB = 2, DCBA = 3 };

    struct PointTableEntry {
        uint32_t    pointId;
        std::string pointName;
        uint32_t    linkId;
        uint8_t     slaveAddress;
        RegisterType regType;
        uint16_t    registerAddr;   // 从站作用域偏移（非全局地址）
        DataType    dataType;
        ByteOrder   byteOrder;
        float       scaleFactor;
        float       offset;
        std::string unit;
        uint32_t    pollIntervalMs;
        uint8_t     priority;
        bool        enabled;
    };
}

// ens/core/crc16.h  —— CRC-16/MODBUS
namespace ens::core {
    // poly 0xA001, init 0xFFFF, 低字节在前（与 HLD-COMM 一致）
    uint16_t crc16_modbus(const uint8_t* data, size_t len) noexcept;
}

// ens/core/mbap.h   —— Modbus TCP 报文头（MBAP）
namespace ens::core {
    struct MbapHeader {
        uint16_t transactionId;
        uint16_t protocolId = 0;
        uint16_t length;
        uint8_t  unitId;
    };
    // 解析/序列化，处理网络字节序（transactionId/length 为大端）
    bool parse_mbap(const uint8_t* p, size_t n, MbapHeader& out) noexcept;
    void emit_mbap(uint8_t* p, const MbapHeader& h) noexcept;
}
```

> 注意：`registerAddr` 是**从站作用域偏移**，与 HLD-SIM §10 寄存器速查表的"从站内偏移"一致；Unit ID（从站地址）单独路由，**切勿**与全局地址混用。

### 2.3 允许的外部依赖

- 唯一允许的第三方依赖：**`nlohmann/json`**（header-only，仅用于解析 JSON 配置 / 点表 / 场景脚本 / 日志）。若要求绝对零依赖，可用 §5 提到的极简 JSON 解析替代，但**不推荐**——`nlohmann/json` 已是事实标准且单头引入。
- 通信：原生 socket（POSIX `socket` / Windows `winsock2`）+ `std::thread`，**不引入** Boost.Asio、POCO 等。
- **GUI 目标依赖**（仅 `src/gui/` 与 `main_gui.cpp`）：允许使用 **Qt 5.15 Widgets**（窗口 / 控件 / 事件循环）。`src/sim/` 引擎模块与 `src/core/` 公共契约**不**链接 QtWidgets，保持零 GUI 依赖（便于隔离单测，NFR-TEST-04）。

---

## 3. 点表数据规格与生成 — 缺口#1

HLD-SIM §3 / §10 给出的是**寄存器 MAP（基址 + 偏移公式）**，但模拟器运行时需要一份**枚举好的点表**（`pointtablePath` 加载）。完整点表 = 由 MAP 机械展开。

### 3.1 展开算法（伪代码）

```
for c in 1..16:                         # BMS 16 簇
    base = 0x1000 + (c-1)*0x600
    簇级寄存器 @ base+0x00..0x0E        # maxTemp/SOC/SOH/avgTemp/totalV/current/
                                        # balanceWord/alarmWord/statusWord
    for i in 0..639:                    # 640 单体电压 @ base+0x10+i
    for i in 0..639:                    # 640 单体温度 @ base+0x290+i
for p in 1..4:                          # PCS 4 台
    base = 0x2000 + (p-1)*0x200
    台级寄存器 @ base+0x00..0x0C
    控制寄存器 排风 @ base+0x1000, 液冷 @ base+0x2000
meter  @ 0x3000 (slave 21)             # 电表
liquid @ 0x4000 (slave 22)             # 液冷机组
fire   @ 0x4100 (slave 23)             # 消防
```

> 全量点表：16×[14 簇级 + 1280 单体] + 4×13 + 3×(少量) ≈ **20 800+** 个 `PointTableEntry`。文件较大，**不应**手写在仓库里。

### 3.2 点表 JSON 文件 schema

```json
{
  "meta": {
    "generator": "ptgen.py",
    "schemaVersion": "1.0",
    "deviceCount": 23,
    "pointCount": 20800
  },
  "points": [
    {
      "pointId": 1, "pointName": "Rack-01_MaxTemp", "linkId": 1,
      "slaveAddress": 1, "regType": "HoldingRegister", "registerAddr": 256,
      "dataType": "Float32", "byteOrder": "ABCD",
      "scaleFactor": 0.1, "offset": 0.0, "unit": "C",
      "pollIntervalMs": 1000, "priority": 1, "enabled": true
    }
    /* ... 其余点 ... */
  ]
}
```

- `regType`/`dataType`/`byteOrder` 用**字符串枚举**（便于 JSON 直读），加载时映射到 §2.2 的 `enum class`。
- `registerAddr` = 从站作用域偏移（十进制或 0x 十六进制均可，加载器统一按 `uint16_t` 解析）。

### 3.3 仓库内交付物

- `data/sim_pointtable_sample.json`：**代表性样例**（Rack-01 全簇级寄存器 + 前 8 个单体电压/温度、PCS#1、Meter、LiquidCooling、Fire），开发人员据此推断全量格式。
- `tools/ptgen.py`：**构建期生成器**（附录 A），CI 或本地 `python tools/ptgen.py > build/sim_pointtable.full.json` 产出全量文件。
- `SimConfig.pointtablePath` 默认指向 `build/sim_pointtable.full.json`；开发期可用 `sample.json` 快速验证。

---

## 4. 物理演化模型常数表 — 缺口#3

LLD-SIM §4 / HLD-SIM §3 给出演化**公式**，本册给出**可填表的常数**（示例值，标注"需按电芯/PCS 规格书校准"，但足以产生可信曲线）。

### 4.1 OCV(SOC) 查表（磷酸铁锂 LFP 示例，单体电压 V）

| SOC% | 0 | 5 | 10 | 20 | 30 | 40 | 50 | 60 | 70 | 80 | 90 | 100 |
|------|---|---|----|----|----|----|----|----|----|----|----|-----|
| OCV(V) | 2.50 | 2.80 | 3.00 | 3.20 | 3.26 | 3.28 | 3.30 | 3.32 | 3.35 | 3.40 | 3.45 | 3.65 |

- 插值：线性插值；<0 或 >100 钳位。
- 此处"示例值"意味着**实现时应替换为真实电芯规格书曲线**，但数据结构与插值算法不变。

### 4.2 热模型系数（HLD-SIM §3 公式 `dT=(k_heat·I²·R_int − k_cool·(T−T_amb))·Δt`）

| 符号 | 含义 | 默认值 | 单位 |
|------|------|--------|------|
| `k_heat` | 发热系数 | 0.0008 | ℃·s/(A²·Ω) |
| `R_int` | 簇等效内阻 | 0.005 | Ω（整簇等效） |
| `k_cool` | 液冷散热系数 | 0.02 | 1/s |
| `T_amb` | 环境温度 | 25.0 | ℃ |
| `balanceCoeff` | 均衡散热附加 | 0.5 | — |

### 4.3 默认初值（启动即合理，避免"全 0 被误判为故障"）

| 量 | 默认 | 单位 | 备注 |
|----|------|------|------|
| 簇 SOC | 80.0 | % | scale 0.01 |
| 簇 SOH | 99.0 | % | |
| 簇最高温 | 35.0 | ℃ | scale 0.1 |
| 簇平均温 | 33.0 | ℃ | |
| 簇总压 | 簇数×3.2×N_series | V | Float32 |
| 簇电流 | 0.0 | A | 可设小幅充/放电 |
| PCS 有功 | 0.0 | kW | |
| PCS 无功 | 0.0 | kVar | |
| PCS 线电压 | 400.0 | V | |
| PCS 频率 | 50.0 | Hz | Float32 |
| PCS 模式 | 0（待机） | — | |
| 告警字/状态字 | 0x0000 | — | 健康 |

### 4.4 积分参数

| 符号 | 默认 | 单位 | 来源 |
|------|------|------|------|
| `Capacity_Ah` | 280.0 | Ah | 单簇标称（示例） |
| `tickMs` | 100 | ms | `SimConfig.tickMs`，与 HLD-SIM §7 一致 |

---

## 5. CMake 工程结构与两目标 — 缺口#4

### 5.1 目标定义

```cmake
# ===== 顶层 EnerSentry/CMakeLists.txt =====
cmake_minimum_required(VERSION 3.16)
project(EnerSentry CXX)
set(CMAKE_CXX_STANDARD 17)
add_subdirectory(apps/device_simulator)   # 定义 DeviceSimulator（Qt GUI）
# add_subdirectory(apps/ens_app)          # 主程序（本册不展开，仅示意）

# ===== apps/device_simulator/CMakeLists.txt =====
find_package(Qt5 COMPONENTS Widgets Core REQUIRED)

# 测试台：纯 Qt 5.15 Widgets GUI 应用（FR-SIM-10）
# 引擎模块（src/sim、src/core）作为本工程内部源文件编译，不单独成库、无 headless 入口
add_executable(DeviceSimulator
    src/main_gui.cpp
    # 引擎模块（纯 C++17，零 Qt）
    src/sim/simulator_engine.cpp src/sim/point_generator.cpp
    src/sim/modbus_slave.cpp src/sim/fault_injector.cpp
    src/sim/scenario_script.cpp src/sim/register_bank.cpp src/sim/sim_config.cpp
    src/core/crc16.cpp src/core/mbap.cpp src/core/point_table.cpp
    # Qt GUI 前端
    src/gui/main_window.cpp src/gui/register_view.cpp
    src/gui/fault_panel.cpp src/gui/scenario_runner.cpp src/gui/log_view.cpp)
target_include_directories(DeviceSimulator PRIVATE src)
target_link_libraries(DeviceSimulator PRIVATE Qt5::Widgets Qt5::Core nlohmann_json::nlohmann_json)
target_compile_features(DeviceSimulator PRIVATE cxx_std_17)
# 引擎模块不链接 Qt（见 §9）：GUI 仅作薄前端消费 RCU 快照
```

### 5.2 编译约束

- C++17；警告 `-Wall -Wextra -Wpedantic`（`-Werror` 可选，建议开发期开启）。
- `nlohmann/json`：通过 `FetchContent` 或 vendored 单头引入（header-only，无链接负担）。
- 跨平台：Windows（MSVC/Clang-cl）+ Linux（gcc/clang）；串口 API 用 `SetupComm/ReadFile/WriteFile`（Win）或 `termios`（POSIX）分支。
- 测试：`enable_testing()` + `add_test`，对应 LLD-SIM §7 用例（UT-SIM-01~14）。

---

## 6. 通信层实现要点（对应 LLD-SIM §3、§4）

### 6.1 ModbusTcpServer（默认 FR-SIM-09，TCP 回环）

- `bind 127.0.0.1:5020`（默认；`SimConfig.tcpPort` 可改），`listen`，`accept` 循环。
- **一个端口服务全部 23 个从站**：MBAP `unitId` 字段路由到对应 `SlaveRegs`（LLD-SIM §4.2 RCU 镜像）。
- 支持功能码：**FC03/04**（读 Holding/Input）、**FC05/06/10**（写单线圈/单寄存器/多寄存器——用于 SBO 控制寄存器回显，验证 FR-CTRL 全链路）。
- `transactionId` 透传（LLD-SIM §3.1），不修改；响应 `length` 字段按大端回填。
- 线程：每个 accept 的连接在 **IO 线程（优先级 HIGHEST）** 处理（LLD-SIM §5.1）；与生成线程通过 `RegisterBank` RCU 解耦（读=快照，写=发布）。

### 6.2 RTU 虚拟串口（与 TCP **同时启用**，承载辅机）

- 通过 `com0com`（Win）/`socat`（Linux）创建虚拟串口对，模拟器打开其中一端（从站）。**该链路与 TCP 监听默认同时拉起**（`SimConfig.rtu.enabled=true`），分别承载液冷（22）/消防（23）辅机——与主程序 RS485 总线部署一致。
- 帧以 **CRC-16/MODBUS** 收尾（§2.2 算法，低字节在前）；解析帧地址/功能码/数据。
- 与 TCP 共用同一 `RegisterBank` 后端，仅传输层差异；链路归属由 `SlaveSpec.transport` 决定（见 LLD-SIM §2.1 / §2.2.4）。
- 故障 FR-SIM-05d 在 RTU 下**翻 CRC 字节**（真实 CRC 失败路径，验证主程序 `crc16ModbusVerify`）；TCP 下无 CRC，等价用"响应破坏"（LLD-SIM §3.4 / HLD-SIM §5.2）。

### 6.3 断链 / 超时模拟（FR-SIM-05c / 05e，对应 LLD-SIM §3.5）

- 05c：对指定 Unit ID/连接停止 accept 或主动 `close()`，使主程序 `TcpChannel` 进入重连（COMM-09 指数退避）。
- 05e：对指定请求延迟 > `responseTimeoutMs` 再回包（验证主程序读超时与熔断 ADR-13）。

---

## 7. 场景脚本样例 — 缺口#5

格式与 HLD-SIM §7 一致：`steps:[{t, action, fault, scope, slave, targetValue, rampRate, durationMs}]`。
仓库内 `scenarios/` 提交以下三个**可直接加载**的脚本（完整内容见对应文件）：

1. **`overheat_drill.json`**（整站过温演练）：t=0 起将全部 16 簇 `maxTemp` 在 60s 内由 35℃ 斜坡拉至 65℃（越限），同时置 `alarmWord.bit0(OverTemp)=1`；t=70s 恢复。验证告警引擎 FR-AL 与温度越限链路。
2. **`random_linkloss_stress.json`**（随机断链压测）：在 300s 窗口内随机对 1~4 个 PCS 从站触发 FR-SIM-05c 断链 5~15s 后恢复，重复多次。验证重连 COMM-09、熔断 ADR-13、SBO 断线清除 FR-CTRL-07。
3. **`voltage_fault_drill.json`**（电压越限演练）：对 Rack-01 前 8 个单体电压在 30s 内拉至 >3.65V（过压）并置 `alarmWord.bit1(CellOverV)=1`；t=40s 恢复。

> 脚本由 `ScenarioScript`（LLD-SIM §2 类 `ScenarioScript`）在 `FaultInjector`（NORMAL 线程）驱动；`t` 为相对场景开始的毫秒偏移，`durationMs` 为作用持续，`rampRate` 为每秒变化量（0=瞬时）。

---

## 8. 观测日志 JSON schema（NFR-TEST-02）— 缺口#7

模拟器运行产出**机器可读 JSON 日志**供 CI 解析，两类：

### 8.1 事件流（`sim_events.jsonl`，每行一个 JSON 对象）

```json
{"ts":"2026-08-14T14:00:00.123Z","level":"INFO","event":"POINT_UPDATE",
 "slave":1,"register":256,"fault":null,"detail":"Rack-01_MaxTemp=35.0C"}
{"ts":"...","level":"WARN","event":"FAULT_INJECT","slave":1,"register":256,
 "fault":"FR-SIM-05a","detail":"overheat ramp start target=65.0C"}
{"ts":"...","level":"ERROR","event":"BAD_FRAME_DROPPED","slave":3,
 "fault":"FR-SIM-05d","detail":"crc mismatch, discarded"}
```

字段：`ts`(ISO8601)、`level`(INFO/WARN/ERROR)、`event`、`slave`、`register`、`fault`(FR-SIM-* 或 null)、`detail`。

### 8.2 汇总报告（`sim_report.json`，场景结束产出）

```json
{
  "scenario": "overheat_drill",
  "durationMs": 70000,
  "faultsInjected": [{"fault":"FR-SIM-05a","count":1,"firstAt":"..."}],
  "badFramesDropped": 0,
  "linkLossEvents": 0,
  "result": "PASS",
  "notes": "alarmWord.bit0 observed set at t=60s"
}
```

`result` ∈ {PASS, FAIL, INCONCLUSIVE}，CI 据此判定。

---

## 9. 线程库决策（缺口#6）：引擎纯 `std::thread`，GUI 用 Qt 事件循环

**结论**：
- **引擎模块（`src/sim/`、`src/core/`）** 仅使用 C++17 标准库 `std::thread` + 原生 socket，**不链接 Qt**——保持 Qt 无关，便于隔离单测（见 §11 验收）。
- **图形启动器 `DeviceSimulator`（FR-SIM-10）** 使用 **Qt 5.15 Widgets** 作为前端：主线程跑 `QApplication` 事件循环；UI 通过 **30Hz `QTimer`** 周期调用 `RegisterBank::snapshot()` 读取最新寄存器镜像并刷新控件（对齐主程序 ADR-22 的渲染节奏），**绝不**在 UI 线程直接读写引擎热路径。

**理由**：
1. 引擎无 UI 依赖，Qt 信号槽无收益；保持纯 C++17 可在无 Qt 环境下编译与单测。
2. GUI 需要窗口 / 控件 / 事件循环，Qt Widgets 与主程序同源（Qt 5.15 LTS），降低学习与维护成本，且天然跨平台（Win / Linux）。
3. 引擎与 Qt 解耦：`src/sim` 不感知 Qt；GUI 仅作为薄前端消费 RCU 快照，避免引擎与 Qt ABI 耦合。

**GUI 与引擎的线程边界**：
- 引擎线程（DataTick / Slave IO / FaultInjector）由 `SimulatorEngine::start()` 在其内部以 `std::thread` 拉起，完全不依赖 `QObject`/`QThread`（LLD-SIM §5）；`stop()` 优雅汇合。
- GUI 向引擎下发指令（启停、注入故障、加载场景）通过 `SimulatorEngine` 的线程安全接口（内部 `std::mutex`/`std::condition_variable` 或队列），**不跨线程直接操作寄存器**。
- 刷新方向单向：引擎 `publish()` 快照 → GUI 轮询 `snapshot()`；GUI 不回写引擎状态。

> **无 headless / 无进程内 Simulation 模式**：你明确只要 GUI 测试台，故不提供命令行 / headless 入口，也不提供主程序链接模拟库的 `ENS_APP_SIM_MODE` 选项。默认拓扑即"两 Qt 工程独立进程 + **Modbus TCP 监听（5020）与 Modbus RTU 虚拟串口双链路同时启用**"（HLD-SIM §2.2 / ADR-SIM-02），主程序日常构建不含任何测试台源码，且经标准 `TcpChannel` / `SerialChannel` 零改动接入。

---

## 10. UI 控制台设计（FR-SIM-10）

图形启动器 `DeviceSimulator` 以 Qt 5.15 Widgets 实现，是测试台的**人机交互前端**；所有模拟 / 故障逻辑仍在 `src/sim/` 引擎模块（纯 C++17），UI 仅消费 RCU 快照与调用引擎线程安全接口（见 §9）。

### 10.1 模块划分

| 类 | 文件 | 职责 |
|----|------|------|
| `SimulatorMainWindow` | `gui/main_window.*` | 主窗口：菜单 / 工具栏（启动·停止·重载配置）、状态栏（监听地址:端口、连接数、运行态） |
| `RegisterView` | `gui/register_view.*` | 设备树（23 从站）+ 实时寄存器表：显示 `pointName` / 工程值 / 原始值 / 时间戳；按 `slaveAddress` 分组，支持按名称 / 告警过滤 |
| `FaultPanel` | `gui/fault_panel.*` | 故障注入控制台：选择故障类型（5a~5e）、作用域（全局 / 从站 / 测点）、触发模式（单次 / 周期 / 随机 / 脚本）、参数（目标值 / 速率 / 持续时间）→ 调用 `SimulatorEngine::injectFault()`；提供"全部恢复"按钮 |
| `ScenarioRunner` | `gui/scenario_runner.*` | 场景脚本运行器：打开 `*.json` → 解析 → 启动 / 暂停 / 停止；进度条（按 `durationMs` 推进）；实时显示当前步骤 |
| `LogView` | `gui/log_view.*` | 事件 / 执行日志查看：订阅引擎事件流（见 §8 `sim_events.jsonl`），按 level 着色、可筛选、可导出 |
| `ConfigPanel` | （`SimulatorMainWindow` 内嵌页） | 配置编辑：`tcp` 端点（enabled/bindIp/port）、`rtu` 端点（enabled/port/baudRate）、每从站 `transport` 归属、`tickMs` / `seed` / `exportLogPath`（NFR-TEST-02 弱化），保存并重载 |

### 10.2 主窗口布局（文本线框）

```
┌──────────────────────────────────────────────────────────────┐
│ DeviceSimulator  [启动] [停止] [重载配置]   127.0.0.1:5020 ●RUN │
├───────────────┬──────────────────────────────┬───────────────┤
│ 设备树         │ 实时寄存器（Rack-01）          │ 事件/日志      │
│ ▸ BMS 1..16   │ MaxTemp   35.2 ℃  [告警]      │ INFO  POINT..  │
│ ▸ PCS 1..4    │ SOC       80.0 %              │ WARN  FAULT..  │
│ ▸ Meter       │ AlarmWord 0x0000             │ ERROR BAD..   │
│ ▸ Aux         │ CellV[0]  3.702 V            │               │
├───────────────┴──────────────────────────────┴───────────────┤
│ 故障注入： [类型▾][作用域▾][模式▾] 目标___ 速率___ 时长___ [注入][恢复] │
│ 场景： [打开…] overheat_drill.json [运行][停止]  进度 [====    ] 60%  │
└──────────────────────────────────────────────────────────────┘
```

### 10.3 交互与刷新模型

- **启动 / 停止**：`ConfigPanel` 提交 `SimConfig` → `SimulatorEngine::start()/stop()`；状态栏随之翻转（IDLE / RUN / ERROR）。
- **实时监视**：`RegisterView` 由主窗口 30Hz `QTimer` 触发，调用 `RegisterBank::snapshot()` 取 `shared_ptr<const SlaveRegs>` 并刷新表格（对齐 ADR-22，避免数据到达即刷新）；只读，不回写引擎。
- **故障注入**：`FaultPanel` 收集参数 → `SimulatorEngine::injectFault(FaultSpec)`；引擎 `FaultInjector` 按状态机（HLD-SIM §5.1）改写 `OverrideTable`，UI 仅发送指令、不直改寄存器。`全部恢复` 调用 `clearAllFaults()`。
- **场景运行**：`ScenarioRunner` 调 `SimulatorEngine::loadScenario(path)` + `run()`；进度由引擎场景时钟驱动，`LogView` 实时显示 `FAULT_INJECT` 等事件；结束产出 `sim_report.json`（§8.2）。
- **配置重载**：`ConfigPanel` 写 `simconfig.json` 并 `engine.applyConfig()`；端口 / 种子 / 步长变更即时生效（NFR-MAINT-03）。

### 10.4 线程与生命周期

- UI 运行于 Qt 主线程（NORMAL）；引擎 DataTick / Slave IO / FaultInjector 仍由各专用线程承载（LLD-SIM §5），UI 卡顿不影响 Modbus 响应（故障隔离，NFR-TEST-04）。
- 关闭窗口 → `QApplication::aboutToQuit` → `engine.stop()` 优雅停止（HLD-SIM §6.5）。

---

## 11. 开发人员验收清单（完成判据）

实现完成以**以下全部可验证**为准（对应 HLD-SIM §8 NFR-TEST / LLD-SIM §7）：

- [ ] `cmake --build` 成功；`DeviceSimulator`（GUI）链接 Qt5::Widgets 可正常启动窗口，引擎模块（`src/sim`）不链接 Qt（§9）。
- [ ] 启动后默认**同时**拉起 TCP 监听（`127.0.0.1:5020`）与虚拟串口 RTU 从站（默认 `COM4`/115200，`tcp.enabled`/`rtu.enabled` 均默认 true）；用 Modbus TCP 客户端读到 BMS/PCS/电表（从站 1~21）合理值，用 Modbus RTU 客户端读到液冷/消防辅机（从站 22/23）合理值，二者均非全 0（FR-SIM-09，完美镜像主程序双栈）。
- [ ] 加载 `sim_pointtable_sample.json` 不报错，点表条目数与样例一致。
- [ ] 运行 `overheat_drill.json`：约 60s 后 `Rack-01_MaxTemp` 寄存器值 > 60℃ 且 `alarmWord.bit0=1`；70s 后恢复。
- [ ] 运行 `random_linkloss_stress.json`：目标 PCS 从站连接中断后恢复，日志出现 `FAULT_INJECT(fault=FR-SIM-05c)`。
- [ ] RTU 链路（虚拟串口，默认随 TCP 同时启用）下注入 FR-SIM-05d：翻 CRC 后主程序侧（或测试客户端）统计到坏帧丢弃（`BAD_FRAME_DROPPED`），验证主程序 `crc16ModbusVerify` 真实失败路径（NFR-REL-03）。
- [ ] 关闭 `rtu.enabled` 可纯 TCP 回归；关闭 `tcp.enabled` 可纯 RTU 回归（单独验证 RTU 的 CRC 失败 / 半双工路径），两条链路互不依赖（§5 / §6）。
- [ ] TCP 模式注入"响应破坏"（FR-SIM-05d 等价）：坏响应被丢弃且计数（NFR-REL-03）。
- [ ] 注入 FR-SIM-05e（超时）：主程序侧读超时触发（依赖主程序就绪）。
- [ ] 指定 `--seed` 两次运行，随机类故障序列**可复现**（NFR-TEST-01）。
- [ ] 运行产出 `sim_events.jsonl` + `sim_report.json`，`result` 字段可被 CI 解析（NFR-TEST-02）。
- [ ] 启动 `DeviceSimulator`（GUI）：主窗口显示监听状态（127.0.0.1:5020）、设备树（23 从站）、实时寄存器值随物理演化刷新（≤30Hz，非全 0）。
- [ ] GUI 故障注入面板：对 Rack-01 触发"过温" → 该簇 `maxTemp` 寄存器与 `alarmWord.bit0` 立即变化；触发"断链" → 连接状态离线；"全部恢复"可复位。
- [ ] GUI 场景运行器：加载 `overheat_drill.json` 并运行 → 进度条推进，结束后 `alarmWord.bit0` 复位；日志视图实时滚动 `FAULT_INJECT` 事件。
- [ ] GUI 配置面板可改 `tcpPort` / `seed` / `tickMs` 并重启生效（NFR-MAINT-03）。

---

## 附录 A：点表生成脚本参考实现（构建期工具，Python）

> 仅用于**生成数据文件**，不是模拟器产品代码。开发人员可直接复制使用或改写。

```python
#!/usr/bin/env python3
# tools/ptgen.py —— 由 HLD-SIM §3/§10 寄存器 MAP 展开全量点表
import json

def gen():
    points, pid = [], 0
    def add(name, link, slave, rtype, addr, dtype, order, scale, unit, prio=1):
        nonlocal pid
        pid += 1
        points.append({
            "pointId": pid, "pointName": name, "linkId": link,
            "slaveAddress": slave, "regType": rtype, "registerAddr": addr,
            "dataType": dtype, "byteOrder": order,
            "scaleFactor": scale, "offset": 0.0, "unit": unit,
            "pollIntervalMs": 1000, "priority": prio, "enabled": True})

    # BMS 16 簇
    for c in range(1, 17):
        base = 0x1000 + (c-1)*0x600
        s = c
        add(f"Rack-{c:02d}_MaxTemp", s, s, "HoldingRegister", base+0x00, "Float32", "ABCD", 0.1, "C")
        add(f"Rack-{c:02d}_SOC",     s, s, "HoldingRegister", base+0x02, "Float32", "ABCD", 0.01, "%")
        add(f"Rack-{c:02d}_SOH",     s, s, "HoldingRegister", base+0x04, "Float32", "ABCD", 0.01, "%")
        add(f"Rack-{c:02d}_AvgTemp", s, s, "HoldingRegister", base+0x06, "Float32", "ABCD", 0.1, "C")
        add(f"Rack-{c:02d}_TotalV",  s, s, "HoldingRegister", base+0x08, "Float32", "ABCD", 0.01, "V")
        add(f"Rack-{c:02d}_Current", s, s, "HoldingRegister", base+0x0A, "Float32", "ABCD", 0.01, "A")
        add(f"Rack-{c:02d}_BalanceWord", s, s, "HoldingRegister", base+0x0C, "Uint16", "ABCD", 1, "bit")
        add(f"Rack-{c:02d}_AlarmWord",   s, s, "HoldingRegister", base+0x0D, "Uint16", "ABCD", 1, "bit")
        add(f"Rack-{c:02d}_StatusWord",  s, s, "HoldingRegister", base+0x0E, "Uint16", "ABCD", 1, "bit")
        for i in range(640):
            add(f"Rack-{c:02d}_CellV_{i:03d}", s, s, "InputRegister", base+0x10+i, "Uint16", "ABCD", 0.001, "V")
        for i in range(640):
            add(f"Rack-{c:02d}_CellT_{i:03d}", s, s, "InputRegister", base+0x290+i, "Uint16", "ABCD", 0.1, "C")
    # PCS 4 台
    for p in range(1, 5):
        base = 0x2000 + (p-1)*0x200
        sp = 10 + p
        for off, nm, dt, sc, un in [(0x00,"ActiveP","Float32",0.01,"kW"),
                                     (0x02,"ReactiveQ","Float32",0.01,"kVar"),
                                     (0x04,"Voltage","Float32",0.01,"V"),
                                     (0x06,"Current","Float32",0.01,"A"),
                                     (0x08,"Freq","Float32",0.01,"Hz"),
                                     (0x0A,"Mode","Uint16",1,""),
                                     (0x0B,"FaultWord","Uint16",1,"bit"),
                                     (0x0C,"Status","Uint16",1,"bit")]:
            add(f"PCS-{p}_"+nm, sp, sp, "HoldingRegister", base+off, dt, "ABCD", sc, un)
        add(f"PCS-{p}_ExhaustCtrl",  sp, sp, "Coil", base+0x1000, "Bool", "ABCD", 1, "")
        add(f"PCS-{p}_LiquidCtrl",   sp, sp, "Coil", base+0x2000, "Bool", "ABCD", 1, "")
    # 电表(21) / 液冷(22) / 消防(23) —— 简化示例，按 ICD 补全
    add("Meter_ActiveEnergy", 30, 21, "InputRegister", 0x3000, "Float32", "ABCD", 0.01, "kWh")
    add("Liquid_SupplyTemp",  31, 22, "InputRegister", 0x4000, "Float32", "ABCD", 0.1, "C")
    add("Fire_Alarm",         32, 23, "DiscreteInput", 0x4100, "Bool", "ABCD", 1, "")
    return points

if __name__ == "__main__":
    pts = gen()
    doc = {"meta": {"generator": "ptgen.py", "schemaVersion": "1.0",
                    "deviceCount": 23, "pointCount": len(pts)}, "points": pts}
    print(json.dumps(doc, ensure_ascii=False, indent=2))
```

---

## 附录 B：与 HLD-SIM / LLD-SIM 章节映射

| 本册章节 | 上游文档 | 关系 |
|----------|----------|------|
| §2 ens::core | LLD-SIM §4.1、ICD §7.1 | 落地类型契约 |
| §3 点表生成 | HLD-SIM §3 / §10 | 由 MAP 展开为数据文件 |
| §4 物理常数 | HLD-SIM §3 演化公式、LLD-SIM §4 | 公式 → 可填表常数 |
| §5 CMake | HLD-SIM §2（形态） | 两目标落地 |
| §6 通信 | LLD-SIM §3 / §4、HLD-SIM §4 | 传输层实现要点 |
| §7 场景 | HLD-SIM §7 | 格式 → 完整脚本 |
| §8 日志 | HLD-SIM §8 NFR-TEST-02 | schema 落地 |
| §9 线程 | LLD-SIM §5、HLD-SIM §6 | 引擎纯 std::thread，GUI 用 Qt 事件循环（无 headless / 无 SimulationMode） |
| §10 UI 控制台 | HLD-SIM §2.2 / §2.5 / §6.1、FR-SIM-10 | 图形前端模块与刷新模型 |
| §11 验收 | HLD-SIM §8、LLD-SIM §7.4 | 完成判据（含 GUI 项） |

*本文档为 ENS-HLD-SIM / ENS-LLD-SIM 的实现规格补充（ENS-SIM-IMP V1.0），与 ICD 点表契约、COMM 通信栈、ENS-CONC-001 线程模型严格一致。主程序通信栈零改动接入铁律（FR-SIM-09 / NFR-TEST-03）在 §5、§6 落实。*
