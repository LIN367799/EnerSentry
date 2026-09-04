# EnerSentry 上位机开发步骤指南（双轨并行版 / V4.0）

> **这是开发的唯一入口文档。** 项目是**单仓库 `EnerSentry` + 两个独立子工程**：
> - **Track A — `apps/ens_app`**：上位机主程序（被测系统）。
> - **Track B — `apps/device_simulator`**：测试台（设备模拟与故障注入，对端）。
>
> 本文把两轨的**完整分步开发计划**写在一起，逐 Phase 配对、互相对接联调。**每个子工程都有自己的步骤清单、骨架代码、测试方法、参考文档**——不是谁附属谁。
> 设计依据：全套 `ENS-HLD-*` / `ENS-LLD-*` / `ENS-(HLD|LLD|SIM)-SIM`、`ENS-BP-000`、`ENS-SRS-000`，以及仓根 `data/` 的数据工件（`sim_pointtable_*.json`、`data/scenarios/*.json`，切片 44a 起收口）与 `04-测试台/tools/ptgen.py`。
> 约定：主程序 L1 通信接入 / L2 协议引擎 / **L3 数据中枢（RingBuffer+DataBus+持久化+黑匣子）** / **L4 业务逻辑（状态机+告警+SBO）** / L5 UI。测试台无 L 编号，逻辑上位于主程序 L1 的「对侧」（被轮询的设备侧）。

---

## 0. 文档地图：开发时该查哪份设计文档

| 你正在写的模块 | 优先查阅的设计文档 | 路径 |
| --- | --- | --- |
| 总体架构 / 线程模型 / 接口契约 | ENS-HLD-000 概要设计总纲 | `02-概要设计/ENS-HLD-000 概要设计说明书.md` |
| **Track A** L1 通信接入（IChannel/累加器/TxId） | ENS-HLD-COMM + ENS-LLD-100 | `02-概要设计/ENS-HLD-COMM 通信接入设计说明.md`、`03-详细设计/ENS-LLD-100 通信接入与协议引擎模块详细设计说明书.md` |
| **Track A** L2 协议引擎（Modbus 引擎/轮询/CRC） | ENS-HLD-PROTO + ENS-LLD-100 + ENS-HLD-ICD | `02-概要设计/ENS-HLD-PROTO 协议引擎设计说明.md`、`03-详细设计/ENS-LLD-100 …`、`02-概要设计/ENS-HLD-ICD 接口控制文档.md` |
| **Track A** L3 数据中枢（RingBuffer/DataBus/SQLite/黑匣子） | ENS-LLD-200 + ENS-HLD-DB + ENS-HLD-THREAD | `03-详细设计/ENS-LLD-200 数据层模块详细设计说明书.md`、`02-概要设计/ENS-HLD-DB 数据库设计说明书（DBDD）.md`、`02-概要设计/ENS-HLD-THREAD 线程模型与并发设计专题报告.md` |
| **Track A** L4 业务逻辑（状态机/告警/SBO） | ENS-HLD-BIZ + ENS-LLD-400 | `02-概要设计/ENS-HLD-BIZ 业务逻辑设计说明.md`、`03-详细设计/ENS-LLD-400 业务层详细设计说明书.md` |
| **Track A** L5 UI（视图/图表/控制） | ENS-HLD-UI + ENS-LLD-500 + ENS-HLD-UX | `02-概要设计/ENS-HLD-UI UI设计说明.md`、`03-详细设计/ENS-LLD-500 UI视图与交互渲染模块详细设计说明书.md`、`02-概要设计/ENS-HLD-UX 交互设计文档.md` |
| **Track B** 测试台整体设计 | ENS-HLD-SIM | `04-测试台/ENS-HLD-SIM 设备模拟与故障注入设计说明.md` |
| **Track B** 测试台详细设计（类/接口/状态机） | ENS-LLD-SIM | `04-测试台/ENS-LLD-SIM 设备模拟与故障注入模块详细设计说明书.md` |
| **Track B** 测试台实现规格补充（工件/CMake/常数/日志） | ENS-SIM-IMP | `04-测试台/ENS-SIM-IMP 设备模拟与故障注入程序实现规格补充.md` |
| 需求 / 蓝图（为什么做） | ENS-SRS-000 + ENS-BP-000 | `01-蓝图与需求/ENS-SRS-000 软件需求规格说明书（SRS）.md`、`01-蓝图与需求/ENS-BP-000 工业上位机实战项目蓝图.md` |
| 点表 / 场景 / 点表生成 | 测试台数据工件 | `data/sim_pointtable_sample.json`、`data/sim_pointtable_full.json`、`data/scenarios/overheat_drill.json`、`04-测试台/tools/ptgen.py` |
| **编码前**（工具链/依赖来源/CMake 骨架/BUILD-0 冒烟） | **ENS-DEV-BOOT 项目启动构建骨架与上手顺序** | `05-开发指南/ENS-DEV-BOOT 项目启动构建骨架与上手顺序.md` |
| 工程目录 / 文件落点 / Target 命名 | ENS-DEV-ARCH 工程目录架构 | `05-开发指南/ENS-DEV-ARCH 工程目录架构.md` |

> 本文 step 里的「📚 参考」即上表对应文档的具体章节，开发到那一步再展开细读，不必一次性读完。  
> **顺序提醒**：动手写业务代码前，先按 `ENS-DEV-BOOT` 把构建系统跑通（BUILD-0 空 Qt 窗口编译/链接/运行全绿），并按 `ENS-DEV-ARCH` 把目录与 CMake Target 对齐，再回到本文逐 Phase 实现。

---

## 1. 总体策略与验证模型

### 1.1 双轨并行：两个子工程，一套 Modbus 方言

单仓库下 `apps/ens_app` 与 `apps/device_simulator` **互不依赖**——主程序编译时根本不需要测试台源码存在（NFR-TEST-03）。两轨通过**统一的字节流契约**对接：主程序 `IChannel`（TCP Client / Serial）↔ 测试台 `ISlaveTransport`（TCP Server / RTU Slave）。

```
Track B (DeviceSimulator)                 Track A (ens_app)
  ModbusTcpServer(5020)  ── TCP ──▶  TcpChannel ──▶ ModbusEngine 解析 ──▶ L1SnapshotStore
  RtuSlavePort(虚拟串口) ─ RTU ──▶  SerialChannel ─▶ 同上
       │                                        │
  PointGenerator 物理演化                       ▼
  FaultInjector 故障注入              DataBus 广播 → 曲线/落库/告警/SBO
```

**推进原则**：两轨**按依赖顺序锁步推进**。先各自完成「最小可联调切片」，再逐层加厚。例如 Track A 写完 `TcpChannel`（Phase 1）的同一周，Track B 必须写完「最小 TCP 从站」（B3），否则 Phase 1 没法验证。本文下方每个 Phase 都给出 **Track A 步骤 + Track B 步骤 + 双轨集成联调**。

### 1.2 三条铁律（写任何一行代码前记住）

1. **层间只经抽象接口 + 信号槽**，禁止跨层 `#include` 实现类（主程序 / 测试台各自内部同理）。
2. **跨线程传自定义类型必须 `qRegisterMetaType`**（否则信号静默丢弃，最难查的 bug 之一）。
3. **任何超时/间隔用 `std::chrono::steady_clock`**，绝不用 `QTime`/`system_clock`（防用户改系统时间导致轮询错乱）。

> 测试台引擎（`src/sim`、`src/core`）是**纯 C++17 零 Qt**，其「跨线程」用的是 `std::thread` + `std::mutex`/`std::condition_variable`，不依赖 Qt 信号槽；GUI 前端（`src/gui`）才用 Qt 事件循环消费 RCU 快照（ENS-SIM-IMP §9）。

### 1.3 三层验证模型（每步的完成定义 DoD）

| 层级 | 名称 | 触发 | 需对端子工程？ | 失败信号 |
| --- | --- | --- | --- | --- |
| **Tier 1** | 编译 / 静态断言 | 每步必过 | 否 | 编译报错、`static_assert` 触发 |
| **Tier 2** | 单元测试（headless） | 每步必过 | 否（可自测） | 断言失败、sanitizer 报错 |
| **Tier 3** | 集成 / 系统联调 | 每切片 / 每 Phase | **是**（跑另一轨） | 日志不符、grep 无匹配、DB 无行 |

`DoD(step) = Tier1 ✓ + Tier2 ✓ + (该步触发的 Tier3 ✓)`。Tier 1/2 必须 100% 自动化、可进 CI；Tier 3 本地手跑 + 关键路径脚本化断言。

### 1.4 Step 0 ── 测试基础设施（两轨地基，最先一起搭）

> 这是「地基的地基」：在写第一行业务代码前就位，后面每步才能即写即验。

**Track A 侧（主程序单测 + 日志规范）**

引入 Catch2：统一经 vcpkg（见仓库根 `vcpkg.json` 的 `catch2` 端口，由 `ens_3rdparty` 收口），无需 vendored 头文件；建 `tests/` 目录：
```
EnerSentry/
└── tests/
    ├── CMakeLists.txt            # add_executable(ens_tests) + catch_discover_tests
    ├── unit/                     # Tier 2：纯 C++17 模块
    │   ├── test_crc16.cpp
    │   ├── test_txid_allocator.cpp
    │   ├── test_accumulator.cpp
    │   ├── test_ringbuffer.cpp
    │   ├── test_downsampler.cpp
    │   └── test_pointtable.cpp
    └── integration/              # Tier 3：链入 src/sim 做 master/slave 回环
        └── test_modbus_loopback.cpp
```
`tests/CMakeLists.txt` 要点（链 `protocol`/`datahub`/`sim_engine`，做回环验证测试台引擎自身）：
```cmake
add_executable(ens_tests unit/test_*.cpp integration/test_modbus_loopback.cpp)
target_link_libraries(ens_tests PRIVATE protocol datahub sim_engine)
target_compile_features(ens_tests PRIVATE cxx_std_17)
include(Catch)
catch_discover_tests(ens_tests)
```
运行：`cmake --build build --target ens_tests && ctest --test-dir build --output-on-failure`

**统一日志规范**（Tier 3 grep 基础）——每层关键路径打结构化日志，字段固定：
```
[<ts>][<layer>][<level>] <event> key=val ...
# 例：
[1700000001][L1][RX ] frame linkId=1 slave=17 fc=03 len=11 crc=ok
[1700000002][L1][ERR] frameError linkId=1 slave=17 kind=crc
[1700000003][L2][OK ] sample pointId=1001 value=48.6
[1700000004][L4][ALM] raised pointId=2003 level=critical reason=overheat
[1700001005][SIM][TX ] fc=03 slave=17 regs=14
```
固定 key：`crc=ok|bad`、`fc=`、`pointId=`、`value=`、`kind=`、`slave=`，联调脚本靠它们判定。

**`qRegisterMetaType` 集中注册**（主程序 `main()` 与 `tests/main.cpp` 都要）：
```cpp
qRegisterMetaType<ens::datahub::Sample>("Sample");
qRegisterMetaType<ens::protocol::ModbusResponse>("ModbusResponse");
qRegisterMetaType<ens::business::AlarmEvent>("AlarmEvent");
qRegisterMetaType<ens::business::SboState>("SboState");
qRegisterMetaType<ens::business::ControlCmd>("ControlCmd");
qRegisterMetaType<ens::datahub::BlackBoxSnapshot>("BlackBoxSnapshot");
```

**Track B 侧（测试台工程脚手架，见 B0）**：在 `apps/device_simulator` 搭 Qt Widgets GUI 空壳（CMake 出 `DeviceSimulator.exe`、能打开窗口、带「启动从站」按钮占位），这是后续 B1~B10 的容器。
- 📚 参考：`ENS-SIM-IMP` §1 代码组织 / §5 CMake / `ENS-HLD-SIM` §4。

---

## 2. Phase 1 ── 通信接入层（L1） + 测试台最小从站

**目标（Track A）**：打通物理链路，稳定收发原始字节流，正确处理 RTU 断包/粘包，分配事务 ID。
**目标（Track B）**：先有「能被主程序连上的最小从站」，让 Track A 一写完就能联调（而非最后才造对端）。

### 2A. Track A 主程序（ens_app）步骤

- [ ] **2.1.1 定义 `ChannelConfig` / `ChannelStats` / `IChannel` 抽象接口**
  - 🔍 测试（Tier 1）：编译通过；`SerialChannel`/`TcpChannel` 两个子类 `override` 全部虚函数，无 `-Werror` 警告。
  - 📚 参考：`ENS-HLD-COMM` IChannel 抽象层 / `ENS-LLD-100` §4.1。↔ 对接 Track B **B0/B1**（从站契约）。
- [ ] **2.1.2 实现 `SerialChannel`（RTU，含 RS485 DE/RE 方向控制）**
  - 🔍 测试（Tier 3，↔ Track B **B4**）：连测试台 RTU 虚拟串口→`bytesReceived==bytesSent`，无丢字节。
  - 📚 参考：`ENS-HLD-COMM` §3.1 RS485 半双工 / `ENS-LLD-100` §4.1.2 / `ENS-SIM-IMP` §6 RTU。
- [ ] **2.1.3 实现 `TcpChannel`（TCP 5020，指数退避重连）**
  - 🔍 测试（Tier 3，↔ Track B **B3**）：连测试台 5020 回显→收发一致；断网→自动重连。
  - 📚 参考：`ENS-HLD-COMM` TCP 链路 / `ENS-LLD-100` §4.1.1 / `ENS-SIM-IMP` §6 TCP。
- [ ] **2.1.4 实现 `ModbusStreamAccumulator`（固定容量环形，零动态分配）**
  - 🔍 测试（Tier 2，`test_accumulator.cpp`）：① 一帧拆 5 小包→恰好出 1 帧且完整；② 2 帧拼接一次喂→出 2 帧；③ 异常帧(`func|0x80`)→按 5/9B 定长立即提取；④ 喂 `kCapacity+100` 字节→溢出覆盖不崩。用 `-fsanitize=address` 验证 `append` 内无堆分配。
  - 📚 参考：`ENS-LLD-100` §4.2 帧处理 / `ENS-HLD-PROTO` 字节流累积。↔ 联调由 Track B **B3/B4** 发粘包/拆包验证。
- [ ] **2.1.5 实现 `TransactionIdAllocator`（`std::bitset<65536>`，O(1)）**
  - 🔍 测试（Tier 2，`test_txid_allocator.cpp`）：连续 `allocate()` 65535 次得 1..65535 全不同；第 65536 次返回 0；`release()` 后可复用；`clearInFlight()` 后全清零。
  - 📚 参考：`ENS-LLD-100` §4.2 TxId 分配器。↔ 联调压测 >65536 请求时由 Track B **B3** 回显配合。
- [ ] **2.1.6 用测试台跑通收发（粘包/拆包/事务边界）**
  - 🔍 测试（Tier 3，↔ Track B **B3/B4**）：① 一次发 3 帧拼接→log `frame` 事件计数=3；② 单帧分 5 包→重组 `crc=ok`；③ 压测 >65536 请求→无重复 ID 告警。
  - 📚 参考：`ENS-HLD-COMM` 联调 / `ENS-SIM-IMP` §6 双链路。

**Track A 关键类骨架**（节选自 `ENS-LLD-100`）
```cpp
// src/channel/IChannel.h
#pragma once
#include <QByteArray>
namespace ens::channel {
struct ChannelStats {                       // 全 atomic，采集线程高频更新
    std::atomic<uint64_t> requestTotal{0}, responseSuccess{0};
    std::atomic<uint64_t> timeoutCount{0}, crcErrorCount{0};
    std::atomic<uint64_t> bytesSent{0}, bytesReceived{0};
    std::atomic<int64_t> avgRttUs{0};
};
class IChannel {
public:
    using ReadCb = std::function<void(const QByteArray&)>;
    virtual ~IChannel() = default;
    virtual bool open() = 0;
    virtual void close() = 0;                       // 幂等
    virtual int  write(const QByteArray& data) = 0; // 非阻塞投递，仅入发送队列即返回
    virtual bool isConnected() const = 0;
    virtual ChannelStats getStats() const = 0;
    void setReadCallback(ReadCb cb) { m_readCb = std::move(cb); }
protected:
    ReadCb m_readCb;
};
}
```
```cpp
// src/protocol/ModbusStreamAccumulator.h
class ModbusStreamAccumulator {
public:
    static constexpr size_t kCapacity = 4096;
    void append(const uint8_t* data, size_t len) noexcept;   // 环形覆盖：溢出丢最旧
    bool tryExtractFrame(uint8_t* out, size_t& outLen, bool isTcp) noexcept;
private:
    std::array<uint8_t, kCapacity> m_buf{};
    size_t m_read = 0, m_write = 0, m_size = 0;
};
// 关键：append 溢出必须前移 m_read；tryExtractFrame 先判 function&0x80 → 异常帧定长立即提取。
```
```cpp
// src/protocol/TransactionIdAllocator.h
class TransactionIdAllocator {
public:
    uint16_t allocate();          // 返回 [1,65535] 未用 ID；无可用返回 0（O(1), bitset）
    void release(uint16_t id);
    void clearInFlight();          // 链路断开时清空在途，防 16-bit 回绕错配
private:
    std::bitset<65536> m_used;     // 0 保留不用
};
```

**Track A 本阶段踩坑**
- ⚠ **RS485 方向控制**：绝不在应用层 `usleep` 模拟 3.5 字符静默；用 `writeCompleted` 释放 DE/RE。
- ⚠ **环形覆盖 bug**：`append` 溢出时必须同步前移 `m_read`，否则读到新旧混合脏数据。
- ⚠ **TCP 无 hunt**：MBAP `Length` 切分，长度不匹配直接丢弃连接缓存并清空累加器 + 触发重连；RTU 才用 HUNT 模式。

### 2B. Track B 测试台（device_simulator）步骤

> 本轨让测试台先有「能被连上的最小从站」。引擎 `src/sim` 纯 C++17（原生 socket + `std::thread`，零 Qt），GUI `src/gui` 仅作薄前端（ENS-SIM-IMP §1、§9）。

- [ ] **B0 工程脚手架（CMake + GUI 空壳）**
  - 建 `apps/device_simulator/CMakeLists.txt`：`add_executable(DeviceSimulator ...)`，链接 `Qt5::Widgets`；源码含 `src/main_gui.cpp` + `src/sim/*` + `src/core/*` + `src/gui/*`（依 ENS-SIM-IMP §5.1）。
  - 建目录 `src/sim/`（引擎）、`src/core/`（公共契约）、`src/gui/`（Qt 前端）、`main_gui.cpp`。
  - `main_gui.cpp`：`QApplication` + `SimulatorMainWindow` 空壳，放 `QPushButton "启动从站"`（先空实现占位）。
  - 🔍 测试（Tier 1）：`cmake --build` 出 `DeviceSimulator.exe`；双击能打开窗口、按钮可点。
  - 📚 参考：`ENS-SIM-IMP` §1 代码组织 / §5.1 目标定义 / `ENS-HLD-SIM` §4。↔ 对接 Track A **2.1.1**。
- [ ] **B1 公共契约 `ens::core` + `crc16_modbus` + MBAP**
  - `src/core/point_table.h`：照搬 `PointTableEntry`/`RegisterType`/`DataType`/`ByteOrder`（与 ENS-LLD-SIM §4.1 逐字节一致，自包含拷贝，不共享编译单元）。
  - `src/core/crc16.h/.cpp`：CRC-16/MODBUS，poly `0xA001`、init `0xFFFF`、低字节在前（`ENS-SIM-IMP` §2.2）。
  - `src/core/mbap.h/.cpp`：`parse_mbap` / `emit_mbap`（大端，transactionId 原样透传）。
  - 🔍 测试（Tier 2）：`crc16_modbus("123456789")=0x4B37`；把本函数纳入 `tests/integration/test_modbus_loopback.cpp` 与主程序 CRC 做 master/slave 回环互逆校验（确保双方方言相同）。
  - 📚 参考：`ENS-SIM-IMP` §2 公共契约 / `ENS-LLD-SIM` §4.1。↔ 对接 Track A **2.1.4/2.1.5** 的 CRC 与字节序。
- [ ] **B3 ModbusTcpServer（最小 TCP 从站，监听 5020）**
  - `src/sim/modbus_slave.*` + `src/sim/modbus_tcp_server.*`：原生 socket `bind 127.0.0.1:5020` + `listen` + `accept` 循环；每个连接一个 IO 线程（HIGHEST）。
  - 内部持 64 寄存器数组；收到 MBAP 请求→查表组响应回包，支持 **FC03/04 读**、**FC06 写回显**（ENS-SIM-IMP §6.1）。
  - 🔍 测试（Tier 2 + Tier 3）：T2 单测 `buildReadResponse` 字节序列正确；T3 用 Python `pyModbusTCP` 或 Track A `TcpChannel` 连 5020，读 64 寄存器得回显值，`bytesSent==bytesReceived`，日志 `RX fc=03 / TX fc=03` 成对。
  - 📚 参考：`ENS-SIM-IMP` §6.1 TCP / `ENS-LLD-SIM` §2.2.4、§4.4。↔ 对接 Track A **2.1.3/2.1.6**（Phase 1 一写完即联调）。
- [ ] **B4 RtuSlavePort（虚拟串口 RTU 从站 + CRC）**
  - `src/sim/rtu_slave_port.*`：通过 com0com（Win）/socat（Linux）虚拟串口对，打开一端作从站（ENS-SIM-IMP §6.2）。
  - 帧以 CRC-16/MODBUS 收尾（低字节在前）；解析地址/功能码/数据。
  - 🔍 测试（Tier 2 + Tier 3）：T2 单测 `buildCorruptFrame` 翻 CRC 字节后 `crc16_modbusVerify` 必失败；T3 连 Track A `SerialChannel`→字节数一致、CRC 互通。
  - 📚 参考：`ENS-SIM-IMP` §6.2 RTU / `ENS-LLD-SIM` §3.4。↔ 对接 Track A **2.1.2**（镜像真实 RS485 辅机链路）。

**Track B 关键类骨架**（节选自 `ENS-LLD-SIM` §4.4 / `ENS-SIM-IMP` §2、§6）
```cpp
// src/core/crc16.h  —— CRC-16/MODBUS（与主程序 crc16ModbusVerify 互逆）
namespace ens::core {
uint16_t crc16_modbus(const uint8_t* data, size_t len) noexcept;  // poly 0xA001, init 0xFFFF
}
// src/core/mbap.h
namespace ens::core {
struct MbapHeader { uint16_t transactionId; uint16_t protocolId = 0; uint16_t length; uint8_t unitId; };
bool parse_mbap(const uint8_t* p, size_t n, MbapHeader& out) noexcept;
void emit_mbap(uint8_t* p, const MbapHeader& h) noexcept;  // length 大端回填
}
// src/sim/ISlaveTransport.h  —— 抽象传输层（TCP 监听 / RTU 从站共用）
namespace ens::sim {
class ISlaveTransport {
public:
    virtual ~ISlaveTransport() = default;
    virtual bool open() = 0;
    virtual void close() noexcept = 0;
    virtual bool isOpen() const noexcept = 0;
    using RequestHandler = std::function<WireFrame(const WireFrame&)>;
    virtual void setRequestHandler(RequestHandler cb) = 0;
};
}
// src/sim/ModbusTcpServer.h  —— 最小 TCP 从站（Sim-Phase A）
class ModbusTcpServer : public ens::sim::ISlaveTransport {
public:
    explicit ModbusTcpServer(std::string ip, uint16_t port);
    bool open() override;                 // bind+listen，accept 循环起 IO 线程
    void close() noexcept override;
    void setRequestHandler(RequestHandler cb) override;
    // 内部：收到 MBAP 请求 → snapshot(slave) → 按 PointTableEntry 编码 → 回包（FC03/04/06）
};
// src/sim/RtuSlavePort.h  —— 虚拟串口 RTU 从站（Sim-Phase A+）
class RtuSlavePort : public ens::sim::ISlaveTransport {
public:
    explicit RtuSlavePort(std::string dev, uint32_t baud);
    bool open() override;                 // 开虚拟串口，挂读循环
    void close() noexcept override;
    void setRequestHandler(RequestHandler cb) override;
    // 内部：收 RTU 帧 → 校验 CRC-16 → 查表编码 → 追加 CRC 回包
};
```
> `WireFrame` 为热路径线缓冲，标 `ENS_CACHE_ALIGN`（16B 对齐，`ENS-LLD-SIM` §4.2）。

**Track B 本阶段踩坑**
- ⚠ **TCP 端口占用**：`bind()` 失败（5020 已占）→ 尝试 `port+1` 重试 ≤3 次；仍失败 `ERROR` 退出（ENS-LLD-SIM §6.1）。
- ⚠ **虚拟串口不存在**：com0com/socat 未就绪 → `RtuSlavePort::open()` 返回 false，记 `ERROR` 并建议切纯 TCP 回归。
- ⚠ **CRC 方言一致**：测试台 CRC 必须和主程序 `crc16ModbusVerify` 完全一致，否则 Tier 3 联调会"假坏帧"。

### 2C. 双轨集成与联调（Phase 1 收口）

| 对接点 | Track A 步骤 | Track B 步骤 | 验证（Tier 3） |
| --- | --- | --- | --- |
| TCP 字节 I/O | 2.1.3 TcpChannel | B3 最小 TCP 从站 | `TcpChannel.bytesSent == Simulator.bytesReceived`；回显场景收发一致；断网→指数退避重连 |
| RTU 字节 I/O | 2.1.2 SerialChannel | B4 RTU 从站 | 虚拟串口对连，`bytesReceived==bytesSent`，无丢字节 |
| 断/粘包 | 2.1.4 累加器 | B3/B4 发 3 帧拼接 / 单帧分 5 包 | log `frame` 计数=3；单帧重组 `crc=ok` |
| 事务边界 | 2.1.5 TxId | B3 回显 | 压测 >65536 请求→无重复 ID 告警 |
| CRC 方言 | 2.1.4（间接） | B1/B4 | `test_modbus_loopback` 回环互逆 |

**断言命令**：见 §9。通过标准：收发计数相等、`crc=ok` 占比 100%（正常场景）、断网后自动重连且 `qualityPercent()>99%`。

---

## 3. Phase 2 ── 协议引擎层（L2）+ 数据模型 + 测试台完整映射

**目标（Track A）**：自研 Modbus 引擎把字节流解析为 `Sample`，点表驱动轮询。
**目标（Track B）**：让测试台加载真实点表铺成完整寄存器映射（FC01~06/15/16），与主程序点表逐字节一致。

### 3A. Track A 主程序（ens_app）步骤

- [ ] **3.1.1 `Crc16` 查表实现（`kCrc16ModbusTable[256]`）**
  - 🔍 测试（Tier 2）：`CRC16("123456789")=0x4B37`；与手工值一致。↔ 与 Track B **B1** CRC 回环互逆。
  - 📚 参考：`ENS-HLD-PROTO` CRC / `ENS-HLD-ICD` 帧格式。
- [ ] **3.1.2 `ModbusFrame`：`buildRequest` / `parseResponse`（FC03/04 读、FC05/06/10 写、异常 0x80+）**
  - 🔍 测试（Tier 2）：组帧字节序列 MBAP+PDU+CRC 正确；正常帧解析得 `value`，`func|0x80` 帧得异常码。
  - 📚 参考：`ENS-HLD-ICD` 功能码与帧结构 / `ENS-LLD-100` §4.2.2。↔ 帧结构即与 Track B **B3/B6** 的契约。
- [ ] **3.1.3 `ModbusEngine`：持有累加器 + TxId 分配器，组帧/解析/分发**
  - 🔍 测试（Tier 3，↔ Track B **B6**）：`Sample.value` 与测试台设定值一致（含缩放）。
  - 📚 参考：`ENS-LLD-100` §4.2.2 / `ENS-HLD-PROTO` / `ENS-LLD-SIM` §4。
- [ ] **3.1.4 `PollScheduler`：半双工串行 / 全双工并发 / BMS 100ms 插队 / 三级熔断降级**
  - 🔍 测试（Tier 2 + Tier 3）：mock「总超时」从机→`getNextPollDelayMs` 阶梯升到 30s、`slaveIsolated` 触发；↔ Track B **B7/B8** 对某从站不回包/延迟触发熔断。
  - 📚 参考：`ENS-LLD-100` §4.3 / `ENS-HLD-COMM` 熔断降级。
- [ ] **3.1.5 `PointTable`：从 `sim_pointtable_sample.json` 加载，提供 `resolve` / `pointIdOf`**
  - 🔍 测试（Tier 2）：`resolve(0x1000, 17)` 得正确 `pointId`；缩放因子还原一致。
  - 📚 参考：`ENS-HLD-ICD` 点表映射 / `ENS-SIM-IMP` §3 / `tools/ptgen.py`。↔ **必须与 Track B B5 加载同一份点表文件**，地址/缩放逐字节一致。
- [ ] **3.1.6 定义 `Sample` / `PointRuntime` 数据模型（16B 对齐）**
  - 🔍 测试（Tier 1）：`static_assert(sizeof(Sample)==16)` 与 `static_assert(is_always_lock_free)` 通过。
  - 📚 参考：`ENS-LLD-200` §3.1 / `ENS-HLD-THREAD` 无锁对齐。
- [ ] **3.1.7 联调（正常/异常/超时/高频）**
  - 🔍 测试（Tier 3，↔ Track B **B6**）：① 正常读→`[L2][OK] sample`；② 测试台发 CRC 错帧→`[L1][ERR] kind=crc` 下游不污染；③ 超时重发→`timeoutCount` 增；④ `sim_pointtable_full.json`+BMS 100ms→10,000+ 测点 `qualityPercent()>99%`。
  - 📚 参考：`ENS-HLD-PROTO` 性能目标 / `ENS-SIM-IMP` §8 压测。

**Track A 关键类骨架**（节选自 `ENS-LLD-100` §4.2.2 / `ENS-LLD-200` §3.1）
```cpp
// src/protocol/ModbusEngine.h
class ModbusEngine : public QObject {
    Q_OBJECT
public:
    explicit ModbusEngine(std::unique_ptr<ens::channel::IChannel> ch, Transport t, QObject* p=nullptr);
    int writeRequest(const ModbusRequest& req, uint32_t linkId);
public slots:
    void onBytesReceived(const uint8_t* raw, size_t len);
signals:
    void responseParsed(uint32_t linkId, uint8_t slaveAddr, const ModbusResponse& resp);
    void frameError(uint32_t linkId, uint8_t slaveAddr, FrameErrorKind kind);
};
// src/datahub/Sample.h
struct ENS_CACHE_ALIGN Sample { uint64_t timestamp; uint32_t pointId; float value; }; // 合计 16B
static_assert(sizeof(Sample)==16);
static_assert(std::atomic<Sample>::is_always_lock_free);
```

**Track A 本阶段踩坑**
- ⚠ **异常帧定长提取**：`function & 0x80` 立即按 5/9 字节提取，别等 `Byte Count`。
- ⚠ **字节序/缩放**：CRC-16/MODBUS 查表、寄存器大端；缩放来自 PointTable，必须与测试台一致。
- ⚠ **点表地址权威化**：用 `tools/ptgen.py` 重新生成的样例（PCS 从站 17~20、BMS 基址 `0x1000+(c-1)*0x600`），旧样例的 `0x100`/从站 11/12 是陈旧数字。

### 3B. Track B 测试台（device_simulator）步骤

- [ ] **B2 `RegisterBank`（RCU 快照）+ `SlaveRegs`**
  - `src/sim/register_bank.*`：采用与主程序 `L1SnapshotStore` 一致的 **RCU 模式**（`shared_ptr<const>` 原子替换，ENS-LLD-SIM §4.2、ADR-LLD-18）。`snapshot()` 无锁读（`shared_lock`），`publish()` 极短 `unique_lock` 写。
  - `SlaveRegs`：`holding`/`input`/`coils`/`discretes` 向量（从站作用域偏移索引）。
  - 🔍 测试（Tier 2）：生成线程高频 `publish` + IO 线程并发 `snapshot`，`-fsanitize=thread` 无 data race；旧快照在最后持有者释放后自动析构。
  - 📚 参考：`ENS-LLD-SIM` §4.2、§5.2。↔ 供后续 B3~B8 共用后端。
- [ ] **B5 `PointGenerator`（物理演化）+ 点表加载**
  - `src/sim/point_generator.*` + `src/sim/sim_config.*`：加载 `sim_pointtable_sample.json`（`SimConfig.pointtablePath`），按 `SlaveSpec` 把点表铺成 23 从站寄存器镜像。
  - `generateTick()`：每 tick 遍历从站，按 HLD-SIM §3.8 物理模型推进（`evolveBms/Pcs/Meter/Aux`），`rng` 以 `SimConfig::seed` 构造（NFR-TEST-01 确定性）。
  - 🔍 测试（Tier 2，`UT-SIM-01/02`）：写入 `raw=3700`（scale 0.001）于单体电压→工程值 = 3.700V；放电电流>0 跑 100 tick→SOC 单调降、温度随 I²R 升；同 `seed` 两次演化序列一致。
  - 📚 参考：`ENS-SIM-IMP` §3 点表 / §4 物理常数 / `ENS-LLD-SIM` §4.5.2。↔ **必须与 Track A B... 3.1.5 同文件**，否则点表错位。
- [ ] **B6 `SimConfig` 加载 + 双链路同时拉起（ModbusSlaveEmulator）**
  - `src/sim/modbus_slave.*`：`ModbusSlaveEmulator` 持 `vector<unique_ptr<ISlaveTransport>>`，依 `SimConfig.tcp/rtu.enabled`（均默认 true）分别 `open()` TCP 监听 + RTU 从站，**共用同一 `RegisterBank`**（`ENS-SIM-IMP` §6、ADR-SIM-02）。
  - 支持 **FC01/02/03/04/05/06/15/16**（读线圈/离散/保持/输入、写单/多线圈/寄存器），按 `PointTableEntry` 的 `dataType/byteOrder` 编码响应。
  - 🔍 测试（Tier 3）：启动后 TCP 5020 + RTU COM4 **同时**监听；用 Modbus 客户端读到 BMS/PCS/电表（从站 1~21）与液冷/消防（22/23）合理值，均非全 0；关闭 `rtu.enabled` 可纯 TCP 回归、关闭 `tcp.enabled` 可纯 RTU 回归（两条链路互不依赖）。
  - 📚 参考：`ENS-SIM-IMP` §1、§5、§6 / `ENS-LLD-SIM` §2.2.4、§4.4。↔ 对接 Track A **3.1.3/3.1.7**（Phase 2 全功能码联调）。

**Track B 关键类骨架**（节选自 `ENS-LLD-SIM` §4.2、§4.5.2）
```cpp
// src/sim/RegisterBank.h  —— RCU 快照库
namespace ens::sim {
class RegisterBank {
public:
    std::shared_ptr<const SlaveRegs> snapshot(uint8_t slave) const;          // 无锁读
    void publish(uint8_t slave, std::shared_ptr<const SlaveRegs> next) noexcept; // 极短写锁
    uint16_t readControl(uint8_t slave, uint16_t reg) const noexcept;
    void     writeControl(uint8_t slave, uint16_t reg, uint16_t v) noexcept;  // SBO 回显
private:
    std::unordered_map<uint8_t, std::shared_ptr<const SlaveRegs>> m_banks;
    mutable std::shared_mutex m_rw;
};
// src/sim/PointGenerator.h  —— 物理演化器
class PointGenerator {
public:
    void generateTick(RegisterBank& bank, FaultInjector& fi, const SimConfig& cfg) noexcept;
private:
    std::mt19937 m_rng;            // 以 SimConfig::seed 构造
    PhysicsParams m_phys;          // OCV 表 / 热系数 / 默认初值（ENS-SIM-IMP §4）
    void evolveBms(SlaveRegs& r, float dtS);
    void evolvePcs(SlaveRegs& r, float dtS);
};
// 生成伪代码（ENS-LLD-SIM §4.5.2）：
//   dtS = tickMs/1000; for each slave: regs = beginSlave(slave);
//   evolve*(regs, dtS); for reg in touched: eff = fi.resolveOverride(slave,reg);
//   if eff.active: regs.setHolding(reg, fromEngineering(eff.value, entry)); if OverTemp setAlarmBit(regs,0);
//   bank.publish(slave, regs);
// src/sim/ModbusSlaveEmulator.h
class ModbusSlaveEmulator {
public:
    explicit ModbusSlaveEmulator(RegisterBank* bank, FaultInjector* fi);
    bool start(const SimConfig& cfg);   // 依 cfg.tcp/rtu.enabled 分别 open 两条链路
    void stop() noexcept;
private:
    std::vector<std::unique_ptr<ISlaveTransport>> m_transports;  // [TcpServer?, RtuSlavePort?]
};
```

**Track B 本阶段踩坑**
- ⚠ **点表作用域**：`registerAddr` 是**从站作用域**偏移，经 MBAP `Unit ID` / RTU 首字节路由，不同从站的 `0x3000` 不冲突（ENS-LLD-SIM §4.2 注）。
- ⚠ **确定性 seed**：固定 `seed` → 同一脚本演化序列可复现，是 NFR-TEST-01 验收硬指标。
- ⚠ **编码一致性**：响应编码用的 `dataType/byteOrder/scaleFactor` 必须和加载的点表逐字段一致，否则主程序解析值错位。

### 3C. 双轨集成与联调（Phase 2 收口）

| 对接点 | Track A 步骤 | Track B 步骤 | 验证（Tier 3） |
| --- | --- | --- | --- |
| 全功能码解析 | 3.1.3 ModbusEngine | B6 完整映射 FC01~06/15/16 | 读写全部功能码，值/缩放与主程序一致 |
| 点表一致 | 3.1.5 PointTable | B5 加载同份 `sim_pointtable_sample.json` | `Sample.value == 测试台设定工程值`（缩放精度内） |
| 轮询/熔断 | 3.1.4 PollScheduler | B7/B8 某站不回包/延迟 | 主程序 `slaveDegraded→slaveIsolated` 阶梯；指数退避重连 |
| 高频压测 | 3.1.7 | B6 全量点表发数 | 10,000+ 测点 `qualityPercent()>99%` |

**断言命令**：见 §9。通过标准：`[L2][OK] sample` 计数随发数增长、`kind=crc` 在正常场景为 0、`timeoutCount` 仅在注入超时时增。

---

## 4. Phase 3 ── 数据中枢（L3）+ 业务逻辑（L4）+ 测试台故障注入

**目标（Track A）**：无锁数据扇出、业务状态机、告警引擎、SBO 控制，打通 SQLite 持久化与黑匣子。
**目标（Track B）**：故障注入引擎 + 坏帧/断链/超时短路，驱动主程序告警/SBO/黑匣子全链路。

### 4A. Track A 主程序（ens_app）步骤

- [ ] **4.1.1 `RingBuffer<T>` 无锁环形缓冲（SPSC + 多消费者 Cursor）**
  - 🔍 测试（Tier 2）：① push 1000 顺序一致；② push `2*Capacity`→`overflowCount` 增、序列号校验跳变；③ `static_assert(is_always_lock_free)`；`-fsanitize=thread` 无 race。
  - 📚 参考：`ENS-LLD-200` §3.2 / `ENS-HLD-THREAD` 无锁模型。
- [ ] **4.1.2 `L1SnapshotStore`：稠密 ID 数组 / 稀疏 QHash 回退**
  - 🔍 测试（Tier 2）：已知 pointId→value 读出一致；越界走稀疏回退不崩。↔ 联调由 Track B **B6** 发数验证快照值。
  - 📚 参考：`ENS-LLD-200` §3.3。
- [ ] **4.1.3 `DataBus`：观察者模式广播（订阅表 `QReadWriteLock` 保护）**
  - 🔍 测试（Tier 2）：订阅通配→push 10 收 10；`unsubscribe` 后不再收。↔ 联调验证 Track B **B6** 发数后 UI/黑匣子/降采样多消费者各收到。
  - 📚 参考：`ENS-LLD-200` §6 / `ENS-HLD-000` §6.5。
- [ ] **4.1.4 `DownSampler`：1s/5s/1m 分桶 + UI Min-Max 降采样**
  - 🔍 测试（Tier 2）：1000 点含尖峰→降到 100 保留最大/最小尖峰。
  - 📚 参考：`ENS-LLD-200` §5 / `ENS-HLD-UI` 渲染优化。
- [ ] **4.1.5 `L2HistoryStore` + `SQLiteDataAccess`：WAL 批量落库、按月分库、跨月 ATTACH**
  - 🔍 测试（Tier 2 + Tier 3）：T2 单月 `history_1s_YYYYMM`(`WITHOUT ROWID`) 有行；T3 跨月 `ATTACH` 后可见、析构 `DETACH`。↔ Track B **B6** 持续发数 10 分钟验证月库路由 + `TransactionGuard` 无半成品。
  - 📚 参考：`ENS-HLD-DB` 月库路由 / `ENS-LLD-200` §4 / `ENS-HLD-THREAD` SQLite WAL。
- [ ] **4.1.6 `BlackBoxManager` + `PlatformMMap`：Critical 级 mmap 即时落盘 + 备份恢复**
  - 🔍 测试（Tier 3）：触发 Critical→`triggerBlackBox` 持锁 <10μs；断电前 30s 快照 mmap 文件可读。↔ **Track B B7 故障注入越限触发**（合并跑 4.3.2）。
  - 📚 参考：`ENS-LLD-200` §7 / `ENS-HLD-BIZ` 极限保护。
- [ ] **4.3.1 `BusinessStateMachine`：Station/Device/Point 配置态/运行态/统计态**
  - 🔍 测试（Tier 2）：三态转换与统计累计正确。↔ Track B **B6** 发数驱动状态迁移。
  - 📚 参考：`ENS-HLD-BIZ` 领域实体 / `ENS-LLD-400` §3。
- [ ] **4.3.2 `AlarmEngine`：迟滞判定、风暴抑制、`MAX_PENDING_STORM=2000` + `droppedCount` 原子**
  - 🔍 测试（Tier 2）：阈值±滞回带抖动不频繁翻转；瞬时灌 5000→`pendingStorm` 截断 2000、`droppedCount` 增、不 OOM。↔ **Track B B8 加载 `overheat_drill.json` 改写温度寄存器触发**（合并跑 4.1.6）。
  - 📚 参考：`ENS-HLD-BIZ` 告警引擎 / `ENS-LLD-400` §4 / `ENS-LLD-SIM` §5。
- [ ] **4.3.3 `SboStateMachine` + `DeviceSboGuard`：Select→Armed(5s)→Operate，断线/超时清锁**
  - 🔍 测试（Tier 2 + Tier 3）：T2 状态机三态；同 `regAddr` 二次请求被 `DeviceSboGuard` 拒；T3 见 Track B B8。
  - 📚 参考：`ENS-HLD-BIZ` SBO / `ENS-LLD-400` §5 / `ENS-LLD-SIM` §4。
- [ ] **4.3.4 联调（SBO / 告警 / 持久化）**
  - 🔍 测试（Tier 3，↔ Track B **B8**）：① UI 发 `ControlCmd`→测试台实测 Select/Arm/Operate，中途断链→Armed 自动清、按钮复位；② `overheat_drill.json`→`[L4][ALM] raised` + 黑匣子落盘；③ 跑 10 分钟→月库生成、`TransactionGuard` 无半成品。
  - 📚 参考：`ENS-LLD-SIM` 故障注入 / `ENS-SIM-IMP` §8。

**Track A 关键类骨架**（节选自 `ENS-LLD-200` §3.2/§6、`ENS-LLD-400` §5）
```cpp
// src/datahub/RingBuffer.h
template <typename T, size_t Capacity>
class RingBuffer {
    static_assert(std::atomic<T>::is_always_lock_free);
    static_assert((Capacity & (Capacity-1)) == 0);
public:
    static constexpr size_t MAX_CONSUMERS = 4;   // [0]UI [1]黑匣子 [2]降采样 [3]预留
    void push(const T& item) noexcept {
        const size_t pos = m_writePos.fetch_add(1, std::memory_order_relaxed);
        m_buffer[pos & MASK].store(item, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        m_publishedPos.store(pos, std::memory_order_release);
    }
    size_t readRecent(int cid, T* out, size_t count) noexcept;
private:
    std::vector<std::atomic<T>> m_buffer;
    std::atomic<size_t> m_writePos{0}, m_publishedPos{0};
    std::array<std::atomic<size_t>, MAX_CONSUMERS> m_consumerCursors{};
};
// src/datahub/IDataAccess.h（L4/L5 只依赖此接口）
class IDataAccess {
public:
    virtual QString getTableName(uint32_t pid, uint64_t ts, HistoryGranularity g) const = 0;
    virtual bool batchInsertHistory(const std::vector<DownSampledSample>&) = 0;
    virtual bool insertBlackBox(const BlackBoxSnapshot&) = 0;
    virtual bool insertAlarm(const AlarmRecord&) = 0;
};
// src/business/SboStateMachine.h
class SboStateMachine : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Armed, Operating };
    void onControlRequest(const ControlCmd& cmd);
signals:
    void sboStateChanged(uint32_t pointId, State);
    void sendFrame(const ModbusRequest&);   // → ModbusEngine 下行写通道
private:
    DeviceSboGuard m_guard;     // 二维 key 防同址并发
    QTimer* m_armTimer;         // 5s 倒计时（本线程持有，勿跨线程）
};
```

**Track A 本阶段踩坑**
- ⚠ **`qRegisterMetaType`**：`Sample`/`ModbusResponse`/`AlarmEvent`/`SboState`/`ControlCmd`/`BlackBoxSnapshot` 跨线程类型须在 `main()` 注册，否则 `QueuedConnection` 静默丢弃。
- ⚠ **慢消费者回卷**：`published - cursor >= Capacity` → 游标强制跳跃 + `slow_consumer_evicted` 告警。
- ⚠ **SQLite 并发**：WAL + `BEGIN IMMEDIATE`（`TransactionGuard` RAII 析构 ROLLBACK）；落库放持久化线程；跨月 `ATTACH` ≤6、析构必 `DETACH`。
- ⚠ **黑匣子**：`triggerBlackBox` 持锁 ~10μs 原子预拷贝后释放，再异步落盘；Windows 文件锁冲突走 backup & recreate。
- ⚠ **告警风暴**：`MAX_PENDING_STORM=2000` 硬上限 + `droppedCount` 原子计数防 OOM。

### 4B. Track B 测试台（device_simulator）步骤

- [ ] **B7 `FaultInjector` + `FaultSession` 状态机（五类故障）**
  - `src/sim/fault_injector.*`：维护 `OverrideTable`（按 `slave→reg` 索引）。`resolveOverride(slave,reg)` 被 `PointGenerator` 每 tick 调用，返回 `FaultEffect`（覆盖值/破坏标志/延迟）。
  - 五类故障：`OverTemp` / `CellVoltage` / `CommLoss` / `CrcError` / `Timeout`（ENS-LLD-SIM §4.3）。
  - `FaultSession` 状态机：`IDLE→ACTIVE→RECOVERING→IDLE`（`ABORTED` 可经 `abort()`），平滑回归基线（呼应 FR-AL-03 迟滞）。
  - 🔍 测试（Tier 2，`UT-SIM-04~11`）：过温注入→该簇最高温寄存器升、告警字 bit0 置位；状态机 `trigger→recover` 平滑回归无突变；`seed` 固定序列可复现。
  - 📚 参考：`ENS-LLD-SIM` §3.2、§4.3、§5.1、§7.2。↔ 对接 Track A **4.3.2/4.3.3/4.1.6**（Phase 3 告警/SBO/黑匣子）。
- [ ] **B8 SBO 写回显 + 坏帧注入 + 断链/超时短路**
  - 在 `ModbusSlaveEmulator::onRequest` 内：① FC05/06/10 写 → `writeControl` 回显（验证 FR-CTRL 全链路，ENS-LLD-SIM §3.3）；② `buildCorruptFrame`：RTU 翻 CRC 字节 / TCP 破坏 PDU 字节（ENS-LLD-SIM §3.4、§4.5.4）；③ `dropLink`：RTU 丢弃请求 / TCP 关该 Unit 连接；④ `delayMs`：延迟 > `responseTimeoutMs` 再回包（ENS-LLD-SIM §3.5）。
  - 🔍 测试（Tier 2 + Tier 3）：T2 `buildCorruptFrame` 翻后 `crc16ModbusVerify` 必失败；T3 主程序 SBO 全流程 Select/Arm/Operate 回显，中途 `CommLoss` 断链→主程序 Armed 自动清锁；`Timeout`→主程序读超时触发（依赖主程序就绪）。
  - 📚 参考：`ENS-LLD-SIM` §3.3/§3.4/§3.5、§4.5.3~4.5.5 / `ENS-SIM-IMP` §6.3。↔ 对接 Track A **4.3.4** 全流程。

**Track B 关键类骨架**（节选自 `ENS-LLD-SIM` §4.3、§4.5.3~4.5.5）
```cpp
// src/sim/fault_injector.h
namespace ens::sim {
enum class FaultType  : uint8_t { OverTemp=0, CellVoltage=1, CommLoss=2, CrcError=3, Timeout=4 };
enum class FaultState : uint8_t { IDLE=0, ACTIVE=1, RECOVERING=2, ABORTED=3 };
struct FaultEffect {
    bool active=false; float value=0;
    bool corruptCrc=false, corruptByte=false, dropLink=false;  // RTU 坏CRC / TCP 坏PDU / 不响应
    int32_t delayMs=0;
};
class FaultInjector {
public:
    FaultEffect resolveOverride(uint8_t slave, uint16_t reg) const;  // PointGenerator 每 tick 调用
    FaultHandle trigger(const FaultRequest& req);
    bool        recover(FaultHandle h) noexcept;
    bool        abort(FaultHandle h) noexcept;
    void        tickSessions(uint32_t dtMs) noexcept;               // FI 线程驱动
private:
    OverrideTable m_table;
    std::vector<FaultSession> m_sessions;
};
}
// onRequest 短路（ENS-LLD-SIM §4.5.5）：
//   eff = fi.resolveOverride(slave, 0xFFFF);
//   if eff.dropLink: return NOTHING;            // 断链：RTU 丢弃 / TCP 关连接
//   if eff.delayMs>0: scheduleLater(...); return NOTHING;  // 超时
//   resp = buildReadResponse(...); if eff.corruptCrc|corruptByte: resp = buildCorruptFrame(resp);
// uint16_t crc16Modbus(const uint8_t* d, size_t n) { uint16_t c=0xFFFF; for(...) { c^=d[i];
//   for(int b=0;b<8;b++){ bool lsb=c&1; c>>=1; if(lsb) c^=0xA001u; } } return c; }  // 线上 lo 先
```

**Track B 本阶段踩坑**
- ⚠ **故障只在覆盖层**：故障值由 `PointGenerator` 生成快照时经 `resolveOverride` 叠加，IO 线程只负责"当前快照值→字节"，不感知故障逻辑（职责单一）。
- ⚠ **RCU 防竞争**：`OverrideTable` 用快照式 `shared_ptr<const>` 替换，读侧无锁，杜绝 FI 写/生成读竞态。
- ⚠ **断链语义差异**：RTU 无连接概念→丢弃请求；TCP→关闭该 Unit 连接，二者都触发主程序重连/熔断（FR-SIM-09 零改动）。

### 4C. 双轨集成与联调（Phase 3 收口）

| 对接点 | Track A 步骤 | Track B 步骤 | 验证（Tier 3） |
| --- | --- | --- | --- |
| 告警全链路 | 4.3.2 AlarmEngine | B7/B8 加载 `overheat_drill.json` | 约 60s 后 `alarmWord.bit0=1` + `[L4][ALM] raised`；70s 复归 |
| SBO 全链路 | 4.3.3/4.3.4 SBO | B8 FC05/06/10 回显 + 中途 `CommLoss` | Select→Armed→Operate 回显；断链→Armed 自动清锁 + 审计 |
| 黑匣子 | 4.1.6 BlackBox | B7 越限触发 Critical | `triggerBlackBox` 持锁 <10μs；mmap 快照可读 |
| 持久化 | 4.1.5 SQLite | B6 持续发数 10 分钟 | 月库按 `getTableName` 路由；`TransactionGuard` 无半成品 |
| CRC 丢弃 | 4.1.x（间接） | B8 `CrcError` 翻 CRC | 主程序 `crcErrorCount++`，数据不污染 |

**断言命令**：见 §9。通过标准：日志出现 `FAULT_INJECT(fault=FR-SIM-05a)`、`[L4][ALM] raised`；黑匣子 DB 有行；月库表存在。

---

## 5. Phase 4 ── UI 展示层（L5）+ 测试台场景回放与日志

**目标（Track A）**：7 大核心视图、QCustomPlot 图表实时绑定、控制面板下发、渲染性能达标。
**目标（Track B）**：场景脚本驱动 + JSON 日志导出（NFR-TEST-02），让 UI 联调可脚本化、可离线断言。

### 5A. Track A 主程序（ens_app）步骤

- [ ] **5.1.1 `MainWindow` + 7 视图（总览/设备/趋势/告警/控制/配置/日志）**
  - 🔍 测试（Tier 3，↔ Track B **B9/B10**）：各视图可开、切换不崩、`connectionChanged` 正确反映链路状态；测试台频繁断链→UI 不卡。
  - 📚 参考：`ENS-HLD-UI` 视图模块 / `ENS-HLD-UX` 交互。
- [ ] **5.1.2 `RealtimePlotWidget`：绑定 `DataBus`/`pointUpdated`，30/60Hz 定时批处理 + 降采样 + `rpQueuedReplot`**
  - 🔍 测试（Tier 2 + Tier 3）：T2 注入 5000 pending→`onBatchRepaint` ≤ `MAX_POINTS_PER_CHANNEL`(2000) 点 `setData`；T3 测试台持续发数→CPU <15%、30Hz 平滑。
  - 📚 参考：`ENS-LLD-200` §5.2 / `ENS-HLD-UI` 图表优化 / ADR-22。↔ 降采样保尖峰用 Track B **B9** 场景回放验证。
- [ ] **5.1.3 `ControlPanel`：绑定 `SboStateMachine::sboStateChanged`，按钮态随状态刷新**
  - 🔍 测试（Tier 3，↔ Track B **B8/B9**）：点按钮→测试台收帧→UI 控制态随 `sboStateChanged` 刷新。
  - 📚 参考：`ENS-HLD-UI` 控制面板 / `ENS-LLD-400` §5 SBO。
- [ ] **5.1.4 渲染优化：OpenGL 降级、High DPI QSS、QSettings 持久化、i18n、隐藏挂起、恢复限清**
  - 🔍 测试（Tier 3）：无独显启动不崩（软件渲染回退）；High DPI 清晰；窗口状态恢复。↔ Track B **B6** 长时间发数验证不崩。
  - 📚 参考：`ENS-HLD-UI` §渲染与兼容性 / `ENS-HLD-UX` 体验。
- [ ] **5.1.5 联调（实时/控制/回放/异常）**
  - 🔍 测试（Tier 3，↔ Track B **B9/B10**）：① 实时刷新流畅；② 控制闭环同步；③ `overheat_drill.json` 回放→告警面板+趋势+黑匣子可查；④ 测试台频繁断链→UI 不卡、`connectionChanged` 正确、恢复后补数。
  - 📚 参考：`ENS-LLD-SIM` 场景回放 / `ENS-SIM-IMP` §10。

**Track A 关键类骨架**（节选自 `ENS-LLD-200` §5.2 / ADR-22）
```cpp
// src/ui/RealtimePlotWidget.cpp —— 禁止数据到达即 replot
class RealtimePlotWidget : public QWidget {
    Q_OBJECT
    static constexpr int MAX_POINTS_PER_CHANNEL = 2000;
public:
    explicit RealtimePlotWidget(QWidget* p=nullptr) {
        m_repaintTimer = new QTimer(this);
        m_repaintTimer->setTimerType(Qt::PreciseTimer);
        connect(m_repaintTimer, &QTimer::timeout, this, &RealtimePlotWidget::onBatchRepaint);
        m_repaintTimer->setInterval(33); m_repaintTimer->start();   // 30Hz
    }
public slots:
    void onNewSample(uint32_t pid, double v, qint64 ts) {  // QueuedConnection
        QWriteLocker lock(&m_buf[pid].rwLock);
        m_buf[pid].pending.append({double(ts), v});         // 仅缓冲，绝不在此 replot
        if (m_buf[pid].pending.size() > 5000) m_buf[pid].pending.remove(0, m_buf[pid].pending.size()-5000);
    }
private slots:
    void onBatchRepaint() {                                 // 定时器触发
        for (auto& [pid, buf] : m_activeChannels) {
            QWriteLocker lock(&buf.rwLock);
            const int target = std::min({MAX_POINTS_PER_CHANNEL, MAX_PIXELS_PER_CHANNEL, width()/int(m_activeChannels.size())});
            buf.ready = (buf.pending.size() > target) ? DownSampler::minMaxBucketDownSample(buf.pending, target) : buf.pending;
            buf.pending.clear();
            m_plot->graph(idx(pid))->setData(/*t*/, /*v*/, /*sorted=*/true);
        }
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    }
};
```

**Track A 本阶段踩坑**
- ⚠ **高频刷新卡顿**：严禁"数据到达即 replot"；统一 QTimer 30/60Hz + 降采样 + `rpQueuedReplot`（ADR-22）。同屏 8 通道 CPU 从 45~60% 降到 8~12%。
- ⚠ **OpenGL 降级**：无独显回退软件渲染；High DPI 集中 QSS；`QSettings` 持久化；i18n 用 `tr()`。
- ⚠ **隐藏挂起**：窗口不可见时挂起 `m_repaintTimer`；恢复时 `m_backlog` 限清防爆内存。

### 5B. Track B 测试台（device_simulator）步骤

- [ ] **B9 `ScenarioScript` 场景驱动 + JSON 日志导出**
  - `src/sim/scenario_script.*`：解析 `data/scenarios/*.json`（`steps:[{t, action, fault, scope, slave, targetValue, rampRate, durationMs}]`，ENS-SIM-IMP §7），按 `t` 时间戳排程驱动 `FaultInjector`；`playLoop` 产出机器可读日志（§8）。
  - 产出 `sim_events.jsonl`（每行一个事件）+ `sim_report.json`（场景结束，`result ∈ {PASS,FAIL,INCONCLUSIVE}`，CI 可解析，NFR-TEST-02）。
  - 🔍 测试（Tier 2 + Tier 3）：T2 单测 `overheat_drill.json` 按 `t` 精确触发最终恢复；T3 跑 `overheat_drill.json`→约 60s `Rack-01_MaxTemp>60℃` 且 `alarmWord.bit0=1`，70s 恢复，`sim_report.json` 的 `result=PASS`；`--seed` 两次运行序列可复现（NFR-TEST-01）。
  - 📚 参考：`ENS-SIM-IMP` §7 场景 / §8 日志 / `ENS-LLD-SIM` §7.4。↔ 对接 Track A **5.1.2/5.1.5**（降采样保尖峰、回放联调）。
- [ ] **B10 GUI 控制台（Qt 5.15 Widgets 前端，FR-SIM-10）**
  - `src/gui/main_window.*` / `register_view.*` / `fault_panel.*` / `scenario_runner.*` / `log_view.*` / `config_panel`（内嵌）：主窗口菜单/工具栏（启动·停止·重载）、状态栏（监听地址:端口、连接数、运行态）；设备树（23 从站）+ 实时寄存器表；故障注入控制台；场景运行器（进度条）；事件日志视图（按 level 着色、可导出）；配置面板（`tcp`/`rtu` 端点、`tickMs`/`seed`/`exportLogPath`，NFR-MAINT-03）。
  - 30Hz `QTimer` 调 `RegisterBank::snapshot()` 刷新控件（对齐 ADR-22，只读、不回写引擎）；故障注入经 `SimulatorEngine::injectFault` 线程安全接口下发（ENS-SIM-IMP §10）。
  - 🔍 测试（Tier 3）：启动→主窗口显示 `127.0.0.1:5020` + 设备树 + 寄存器值随演化刷新（≤30Hz，非全 0）；故障面板对 Rack-01 触发"过温"→寄存器与 `alarmWord.bit0` 立即变；场景运行器加载 `overheat_drill.json`→进度条推进、结束 `alarmWord.bit0` 复位、日志滚动 `FAULT_INJECT`；配置面板改 `tcpPort`/`seed`/`tickMs` 重启生效。
  - 📚 参考：`ENS-SIM-IMP` §10 UI 控制台 / `ENS-HLD-SIM` §2.2/§6.1。↔ 对接 Track A **5.1.1~5.1.5**（控制闭环、断链恢复、回放）。

**Track B 关键类骨架**（节选自 `ENS-LLD-SIM` §2.2.6、`ENS-SIM-IMP` §10）
```cpp
// src/sim/scenario_script.h
namespace ens::sim {
class ScenarioScript {
public:
    bool load(const std::string& path);          // 解析 steps[]
    bool schedule(FaultInjector* fi);            // 按 t 时间戳排程
    void playLoop();                             // 驱动 FI，产出 sim_events.jsonl / sim_report.json
    std::string name;
    std::vector<Step> steps;
};
// src/sim/simulator_engine.h  —— 编排者（GUI 经其线程安全接口下发指令）
class SimulatorEngine {
public:
    bool start() noexcept;                       // DataTick → Slave IO → FaultInjector 顺序起
    void stop() noexcept;                        // 逆序优雅关闭
    bool loadScenario(const std::string& path);
    FaultHandle injectFault(const FaultRequest& req);
    void clearAllFaults() noexcept;
    RegisterBank* bank() noexcept { return m_bank.get(); }
private:
    std::unique_ptr<RegisterBank> m_bank;
    std::unique_ptr<PointGenerator> m_gen;
    std::unique_ptr<ModbusSlaveEmulator> m_slave;
    std::unique_ptr<FaultInjector> m_injector;
    std::unique_ptr<ScenarioScript> m_scenario;
};
// src/gui/main_window.*  —— 30Hz 轮询快照刷新（ENS-SIM-IMP §9/§10）
//   主线程 QApplication 事件循环；RegisterView 由 30Hz QTimer 调 RegisterBank::snapshot()；
//   FaultPanel → SimulatorEngine::injectFault()；ScenarioRunner → loadScenario()+run()。
```

**Track B 本阶段踩坑**
- ⚠ **无 headless / 无 SimulationMode**：你明确只要 GUI 测试台，故不提供 CLI/headless 入口，也不提供主程序链接模拟库的选项（ENS-SIM-IMP §9）。但其 `src/sim` 引擎零 Qt，仍可链入 `ens_tests` 做回环单测。
- ⚠ **刷新单向**：引擎 `publish()` 快照 → GUI 轮询 `snapshot()`；GUI 不回写引擎状态，避免 UI 线程直读热路径。
- ⚠ **关闭即停**：`QApplication::aboutToQuit` → `engine.stop()` 优雅停止（HLD-SIM §6.5）。

### 5C. 双轨集成与联调（Phase 4 收口）

| 对接点 | Track A 步骤 | Track B 步骤 | 验证（Tier 3） |
| --- | --- | --- | --- |
| 实时绑定 | 5.1.2 图表 | B6/B9 持续发数 / 场景回放 | 30Hz 平滑、降采样保尖峰、CPU <15% |
| 控制闭环 | 5.1.3 ControlPanel | B8/B10 SBO 回显 + 面板注入 | 点按钮→测试台收帧→UI 态与图表同步刷新 |
| 回放/异常 | 5.1.5 联调 | B9/B10 overheat 回放 + 频繁断链 | 告警面板+趋势+黑匣子可查；断链→UI 不卡、恢复补数 |
| 离线断言 | （CI） | B9 导出 `sim_report.json` | `result=PASS` 可被 CI 解析（NFR-TEST-02） |

**断言命令**：见 §9。通过标准：图表 30Hz 平滑且 CPU 达标；`sim_report.json` 的 `result=PASS`；UI 断链不卡、`connectionChanged` 正确。

---

## 6. 跨 Phase 联调资源表

| 资源 | 路径 | 用途 | 📚 参考 |
| --- | --- | --- | --- |
| 设备模拟器工程 | `04-测试台/`（→ `apps/device_simulator`） | TCP 5020 + RTU 虚拟串口双链路，模拟 23 从站 | `ENS-HLD-SIM` |
| 联调点表 | `data/sim_pointtable_sample.json` | 常规联调（43 点，地址已权威化） | `ENS-SIM-IMP` §3 |
| 压测点表 | `data/sim_pointtable_full.json` | 高频压力（~20,667 点） | `ENS-SIM-IMP` §8 |
| 演练场景 | `data/scenarios/overheat_drill.json` | 告警演练 / 场景回放 | `ENS-LLD-SIM` §5 |
| 点表生成 | `04-测试台/tools/ptgen.py` | 自定义点表规模 | `ENS-SIM-IMP` §3 |

> 主程序与测试台经**同一份 `sim_pointtable_sample.json`** 与标准 `IChannel`/`ISlaveTransport` 零改动对接（FR-SIM-09 / NFR-TEST-03）。

---

## 7. 关键踩坑速查（10 条，两轨通用）

1. **`qRegisterMetaType`**（主程序）：跨线程 `QueuedConnection` 自定义类型必须注册，否则信号静默丢弃。
2. **高频刷新**（主程序 L5）：统一 QTimer 30/60Hz + 降采样 + `rpQueuedReplot`，禁数据到达即 replot。
3. **跨线程 QObject 生命周期**（主程序）：勿跨线程持有 `QTimer*`；倒计时用本线程 QTimer 或 `steady_clock`。
4. **时钟跳变**：超时/间隔一律 `std::chrono::steady_clock`，禁 `QTime`/`system_clock`。
5. **无锁 Ring Buffer / RCU**（两轨共有思路）：`Sample` `alignas(16)` + `is_always_lock_free`；`RegisterBank`/`L1SnapshotStore` 用 `shared_ptr<const>` 原子替换，读侧无锁。
6. **SQLite 并发**（主程序 L3）：WAL + `BEGIN IMMEDIATE` + `TransactionGuard` RAII；跨月 `AttachGuard` RAII。
7. **Modbus CRC/字节序**（两轨必须一致）：CRC-16/MODBUS 查表；大端；缩放因子来自点表，解析与模拟须一致（回环单测兜底）。
8. **RS485 半双工 / TCP 事务**（主程序 L1 / 测试台 B3/B4）：RS485 发期间禁并发读，`writeCompleted` 释放 DE/RE；TCP 用 `TransactionIdAllocator` 匹配请求/响应。
9. **SBO 安全**（主程序 L4 / 测试台 B8）：Armed 5s 超时/断链清锁；`DeviceSboGuard` 二维 key 防同址并发；UI 在 Armed 期间禁用其他操作。
10. **资源边界**：累加器固定容量零动态分配（主程序）；`AlarmEngine` `MAX_PENDING_STORM=2000`+`droppedCount`（主程序）；`seed` 确定性（测试台 NFR-TEST-01）；`--seed` 两次可复现。

---

## 8. 推进节奏表（双轨锁步，参考）

| 周次 | Track A（ens_app） | Track B（device_simulator） | 收口联调（查 §对应） |
| --- | --- | --- | --- |
| W1 | §1.4 脚手架 + Phase1（2.1.x） | §1.4 B0 + B1 + B3 + B4 | §2C TCP/RTU 收发一致、粘包/拆包 |
| W2 | Phase2（3.1.x） | B2 + B5 + B6 | §3C 全功能码 + 点表逐字节一致 + 熔断 |
| W3 | Phase3 L3 数据中枢（4.1.x） | B6 持续发数 10 分钟 | §4C 持久化月库 + 黑匣子可读 |
| W4 | Phase3 L4 业务（4.3.x） | B7 + B8 | §4C SBO 全流程 + overheat 演练 |
| W5 | Phase4 视图 + 图表降采样 + 控制（5.1.x） | B9 + B10 | §5C 实时刷新 + 控制闭环 + 断链恢复 |
| W6+ | 长稳烤机、弱网丢包、i18n/High DPI 收尾 | B9 场景压测 + JSON 日志 CI | 稳定性 + CI 回归 |

> **用法**：每完成一个 step，回本文对应 §x.x 的「🔍 测试」三档打勾；两轨同 Phase 的步骤要**一起**完成再收口联调（§xC）。遇到设计分歧，按「📚 参考」查对应设计文档。

---

## 9. 常用验证命令速查

```bash
# === Tier 2 单测（进 CI，两轨共链 ens_tests）===
cmake -S . -B build && cmake --build build --target ens_tests
ctest --test-dir build --output-on-failure

# 内存/数据竞争（开发期抽查，慢）：验证 RingBuffer/RegisterBank RCU 无 race
cmake --build build --target ens_tests && ./build/tests/ens_tests --sanitizers=address,thread

# === Tier 3 联调日志断言（主程序 ens_app.log）===
grep -c "\[L2\]\[OK \] sample" ens_app.log      # 解析成功计数
grep "kind=crc" ens_app.log | wc -l            # CRC 错误数（正常场景应为 0）
grep "slaveIsolated" ens_app.log               # 熔断隔离事件
grep "FAULT_INJECT" simulator.log              # 测试台故障注入事件（B7/B8/B9）

# 持久化校验
sqlite3 history_1s_2026XX.db "SELECT count(*) FROM history_1s_2026XX;"
sqlite3 blackbox.db "SELECT * FROM blackbox ORDER BY ts DESC LIMIT 1;"  # 断电前 30s

# 测试台场景回放 CI 断言（B9）
python -c "import json;d=json.load(open('sim_report.json'));assert d['result']=='PASS'"
```

---

> 本文档为 EnerSentry 单仓库双子工程（Track A `ens_app` + Track B `device_simulator`）的统一开发入口，与全套 ENS-HLD/LLD/SIM 设计、ENS-BP/SRS 蓝图严格一致；主程序通信栈零改动接入铁律（FR-SIM-09 / NFR-TEST-03）贯穿各 Phase 联调。
