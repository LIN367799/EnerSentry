# EnerSentry 项目启动：构建骨架与上手顺序（ENS-DEV-BOOT）

> **文档编号**：ENS-DEV-BOOT  
> **版本**：V1.0  
> **日期**：2026-08-20  
> **定位**：本文是 `ENS-DEV-ARCH`（目录/Target 落点）与 `ENS-DEV-GUIDE`（逐 Phase 业务步骤）的**前置配套**。你已按 `ENS-DEV-ARCH` 建好空目录，本文回答：**在写第一行业务代码之前，先把构建系统跑通、跑绿，应该怎么排顺序、每步怎么验证**——重点包括你关心的「vcpkg 要不要先弄、还是先弄别的」。  
> **主角**：两个子工程 `apps/ens_app`（主程序）+ `apps/device_simulator`（测试台），外加仓库根 `CMakeLists.txt` / `ens_3rdparty` 接口库 / `tests/`。

---

## 0. 一句话结论（先回答你最关心的问题）

> **依赖统一经 vcpkg 引入（Qt5 与小库全由 vcpkg 装），这是编码前必须最先落实的一步——但它不是唯一要做的事。**

正确顺序是：

1. **锁工具链**（编译器/CMake/Qt 版本必须对齐）—— 非此不可，否则后面全是玄学报错。
2. **落实依赖（统一 vcpkg）**：仓库根放 `vcpkg.json` 声明全部依赖（Qt5 + QCustomPlot/Catch2/nlohmann_json/spdlog），配置时注入 vcpkg toolchain 即可，本地与 CI 完全一致。
3. **写构建骨架**：顶层 `CMakeLists.txt` + 各子工程 `CMakeLists.txt` + `ens_3rdparty` 接口库。
4. **跑通 BUILD-0 冒烟里程碑**：一个**空的 Qt 窗口能编译、能链接、能运行**，全绿。
5. **然后才正式编码** —— 接 `ENS-DEV-GUIDE` 的 §1.4 测试地基 + Phase 1~4。

**铁律：BUILD-0 没绿之前，一行业务代码都别写。** 否则你会长期在「功能 bug」和「构建环境 bug」之间来回撕，极难定位。

---

## 1. Step A — 锁工具链（非此不可）

| 工具 | 要求 | 为什么 |
| --- | --- | --- |
| **MSVC 工具集** | VS2019/2022，工具集 ≥ v142 | Qt 5.15.2 的 `msvc2019_64` 预编译包**只能用对应 MSVC 链接**；MinGW 版 Qt 要用 MinGW 编译器，二者不可混。 |
| **CMake** | ≥ 3.21 | vcpkg manifest 模式、Qt 的 CMake 集成需要较新版本。 |
| **Ninja**（可选但推荐） | 最新 | 比 VS generator 快、错误更清晰；配合 `CMAKE_BUILD_TYPE` 用。 |
| **Qt 5.15.2 LTS** | 记下安装路径，如 `C:\Qt\5.15.2\msvc2019_64` | 本项目钉死 Qt 5.15（LGPL 必须 SHARED 动态链接，见 `ENS-DEV-ARCH` §4）。 |
| **Git** | 任意 | 版本控制（vcpkg 自身拉取/构建依赖时也依赖 Git）。 |
| **Python 3.x** | 系统或托管运行时 | 跑 `04-测试台/tools/ptgen.py` 生成点表、跑 `sim_report.json` 断言。 |

✅ **验证（每装一个就验一个，别攒到最后）**：

```bash
cmake --version                 # ≥ 3.21
# 在 Qt 安装目录的 bin 下：
qmake --version                 # 应显示 QMake version 3.1 / Qt 5.15.2
# 打开「VS 开发人员命令提示符」或加载 vcvars：
cl.exe                          # 应显示 Microsoft (R) C/C++ Optimizing Compiler
```

> ⚠ **Qt 编译器必须与 MSVC 版本匹配**：用 `msvc2019_64` 的 Qt，就对应 VS2019 的 v142 工具集；用 VS2022 默认 v143 也能链（ABI 兼容），但别拿 MinGW 的 Qt 去配 MSVC，必崩。

---

## 2. Step B — 依赖从哪来：统一 vcpkg（你问的核心）

**结论先行**：本项目所有第三方依赖（Qt 5.15 + QCustomPlot + Catch2 + nlohmann_json + spdlog + SQLite）**统一经 vcpkg 引入**，不再区分"开发期 FetchContent / 产线 vcpkg"。理由：依赖来源单一、版本可锁定（`vcpkg.json` + `builtin-baseline`），本地 / CI / 产线完全一致、可一键复现；且 Qt 经 vcpkg 默认即 SHARED 动态链接，正好满足 LGPL 红线（§4）。

| 依赖 | vcpkg 端口 | 理由 |
| --- | --- | --- |
| **Qt 5.15（Core/Gui/Widgets/SerialPort/Network）** | `qt5-base` + `qt5-serialport` | LGPL 必须 SHARED，vcpkg 默认动态链接，契合 §4。 |
| **QCustomPlot** | `qcustomplot` | 绘图控件，CMake 友好。 |
| **Catch2** | `catch2` | 单测框架，提供 `Catch2::Catch2WithMain`。 |
| **nlohmann_json** | `nlohmann-json` | JSON 解析，header-only。 |
| **spdlog** | `spdlog` | 运行时日志。 |
| **SQLite3** | `sqlite3`（可选，否则用 Qt `QSql`） | 持久化；本项目优先 Qt SQL。 |

### 2.1 落地步骤

**(1) 安装 vcpkg（一次性）**
```bash
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg && bootstrap-vcpkg.bat        # Windows；Linux/macOS 用 ./bootstrap-vcpkg.sh
```

**(2) 仓库根放 `vcpkg.json` 声明依赖**（manifest 模式，CMake 配置时 vcpkg 自动拉取/构建）
```json
{
  "name": "enersentry",
  "version": "1.0.0",
  "dependencies": [
    "qt5-base",
    "qt5-serialport",
    "qcustomplot",
    "catch2",
    "nlohmann-json",
    "spdlog"
  ],
  "builtin-baseline": "<填入你锁定的 vcpkg commit SHA，用于版本固定>"
}
```
> 端口名以 `vcpkg search <关键词>` 为准（如 `qcustomplot`、`qt5-serialport`）；`builtin-baseline` 建议填一个已知稳定的 vcpkg 提交 SHA，保证团队/CI 装的版本一致。

**(3) 配置时注入 vcpkg 工具链**
```bash
cmake -S . -B build ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --target ens_app
```
> 一旦设了 `CMAKE_TOOLCHAIN_FILE`，vcpkg 会把所有依赖的 CMake 配置包（Qt5、qcustomplot、Catch2、nlohmann_json、spdlog）**自动注入查找路径**，无需再设 `CMAKE_PREFIX_PATH`。首次构建 Qt 等较重依赖较慢，**务必开启 vcpkg binary caching**（默认已启用本地缓存）复用已编产物。

> 不论依赖怎么来，`ens_3rdparty` 这一个 INTERFACE 库都是**唯一的依赖收口点**（见 §4），业务代码永远只 `target_link_libraries(... PRIVATE ens_3rdparty)`，不关心依赖细节。

---

## 3. Step C — 仓库 Bootstrap 文件

### 3.1 `.gitignore`（仓库根，避免把产物/密钥入库）

```gitignore
# 构建产物（out-of-source）
build/
out/
bin/

# Qt Creator / VS 用户文件
*.user
*.user.*
.vs/
.vscode/

# 运行期生成数据（含 meta.db / 审计库 / 历史库，绝不入库）
data/

# 杂项
compile_commands.json
```

### 3.2 顶层 `CMakeLists.txt`（仓库根，最小可配置骨架）

```cmake
cmake_minimum_required(VERSION 3.21)
project(EnerSentry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Qt 自动处理 moc / uic / qrc（与 ENS-DEV-ARCH §2.2 约定一致）
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# 让最终 exe/DLL 集中到仓库根 bin/（ENS-DEV-ARCH §2.2.2）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

enable_testing()                       # 为 CTest + catch_discover_tests 准备
include(CTest)

# 依赖唯一收口（§4）
include(cmake/Ens3rdparty.cmake)

# 子工程开关（默认全开；CI 可 -DBUILD_SIMULATOR=OFF 只测主程序）
option(BUILD_APP        "Build ens_app (主程序)"        ON)
option(BUILD_SIMULATOR "Build device_simulator (测试台)" ON)
option(BUILD_TESTS     "Build ens_tests (单测)"          ON)

if(BUILD_APP)
    add_subdirectory(apps/ens_app)
endif()
if(BUILD_SIMULATOR)
    add_subdirectory(apps/device_simulator)
endif()
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
```

✅ **验证**：在仓库根 `cmake -S . -B build`（此时子目录还是空壳也行，只要各子工程 `CMakeLists.txt` 存在且至少 `add_subdirectory` 或空文件）——配置**零报错**即通过。

---

## 4. Step D — `ens_3rdparty` 接口库（依赖唯一收口）

建 `cmake/Ens3rdparty.cmake`，把 §2 的所有依赖在这里一次性 `find_package`（vcpkg 提供 CMake 配置包），再装进一个 INTERFACE 库：

```cmake
# cmake/Ens3rdparty.cmake
# 所有第三方依赖统一经 vcpkg 提供（manifest 模式，见仓库根 vcpkg.json）
# vcpkg 通过 CMAKE_TOOLCHAIN_FILE 注入 find_package 所需的 CMake 配置包
add_library(ens_3rdparty INTERFACE)
add_library(ens::3rdparty ALIAS ens_3rdparty)

# —— Qt 5（vcpkg 端口 qt5-base + qt5-serialport）——
find_package(Qt5 COMPONENTS Core Gui Widgets SerialPort Network REQUIRED)

# —— 小库（vcpkg 端口）——
find_package(QCustomPlot REQUIRED)      # 目标 qcustomplot::qcustomplot（端口名以 vcpkg search 为准）
find_package(Catch2 REQUIRED)           # 目标 Catch2::Catch2WithMain
find_package(nlohmann_json REQUIRED)    # 目标 nlohmann_json::nlohmann_json
find_package(spdlog REQUIRED)           # 目标 spdlog::spdlog

# —— 统一收口 ——
target_link_libraries(ens_3rdparty INTERFACE
    Qt5::Core Qt5::Gui Qt5::Widgets Qt5::SerialPort Qt5::Network
    qcustomplot::qcustomplot
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    spdlog::spdlog)
# 头文件搜索路径也一并透传
target_include_directories(ens_3rdparty INTERFACE
    ${CMAKE_SOURCE_DIR}/apps/ens_app/include
    ${CMAKE_SOURCE_DIR}/apps/device_simulator/include)
```

✅ **验证**：写一个临时 `add_executable(_probe src/probe.cpp)` 只 `target_link_libraries(_probe PRIVATE ens_3rdparty)` 且 `probe.cpp` 里 `#include <QWidget>` + `#include <nlohmann/json.hpp>`，能编译通过即说明依赖收口生效。（验证完删掉 probe。）

---

## 5. Step E — 各子工程 `CMakeLists.txt`（最小骨架）

### 5.1 `apps/ens_app/CMakeLists.txt`

```cmake
# 主程序（被测系统）
# 后续按 ENS-DEV-ARCH 把 src/channel src/protocol ... 加进各 target
qt_add_executable(ens_app           # Qt5 下用 add_executable 亦可，AUTOMOC 已全局开
    src/app/main.cpp)               # ← BUILD-0 阶段只有这一个文件
target_link_libraries(ens_app PRIVATE ens::3rdparty)
```

> 注：Qt 5.15 用 `qt5_add_executable` 或原生 `add_executable` 均可；本仓库已全局开 `CMAKE_AUTOMOC/AUTOUIC/AUTORCC`，无需每个 target 再设。

### 5.2 `apps/device_simulator/CMakeLists.txt`

```cmake
# 测试台（对端）。BUILD-0 阶段先只链 Qt；引擎/前端就绪后补 sim_engine / sim_gui
qt_add_executable(DeviceSimulator
    src/main_gui.cpp)               # ← BUILD-0 阶段只有 GUI 空壳
target_link_libraries(DeviceSimulator PRIVATE ens::3rdparty)
# 后续：target_link_libraries(DeviceSimulator PRIVATE sim_engine sim_gui)
```

### 5.3 `tests/CMakeLists.txt`

参考 `ENS-DEV-GUIDE` §1.4 的片段：`add_executable(ens_tests ...)` + `target_link_libraries(ens_tests PRIVATE ens::3rdparty protocol datahub sim_engine)` + `include(Catch)` + `catch_discover_tests(ens_tests)`。BUILD-0 阶段可先只挂一个空的 `tests/unit/test_sanity.cpp`（一个 `TEST_CASE` 直接 `REQUIRE(true)`）验证注册链路。

✅ **验证**：`cmake -S . -B build` 配置成功，三个 target（`ens_app` / `DeviceSimulator` / `ens_tests`）都出现在生成的项目里。

---

## 6. Step F — BUILD-0 冒烟里程碑（最重要的一关）

**目标**：一个**空的 Qt 窗口**能编译、链接、运行，全绿。它本身不含任何业务逻辑，只验证「编译器 + Qt + CMake 三自动 + 链接 + 部署」整条链路通了。

`apps/ens_app/src/app/main.cpp`（含我们前面定好的启动顺序：先登录、后主窗）：

```cpp
#include <QApplication>
#include <QMainWindow>
#include <QLabel>
#include <QDialog>
#include <QDialogButtonBox>

// BUILD-0 暂用 stub 登录框：直接返回 Accepted，后续替换为 ENS-LLD-500 §7 的 LoginDialog
class LoginDialog : public QDialog {
public:
    LoginDialog(QWidget* p = nullptr) : QDialog(p) {
        setWindowTitle("EnerSentry - Login (stub)");
        auto* lay = new QVBoxLayout(this);
        lay->addWidget(new QLabel("Login stub — BUILD-0"));
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok);
        lay->addWidget(bb);
        connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    }
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    // 启动顺序（ENS-LLD-500 §7 / ENS-DEV-ARCH §3.5）：先登录，成功再显主窗
    LoginDialog dlg;
    if (dlg.exec() != QDialog::Accepted) return 0;

    QMainWindow w;
    w.setWindowTitle("EnerSentry");
    w.setCentralWidget(new QLabel("EnerSentry boot OK — BUILD-0 passed"));
    w.resize(800, 600);
    w.show();
    return app.exec();
}
```

构建与运行：

```bash
cmake --build build --target ens_app
# 开发期让 exe 找得到 Qt DLL：把 Qt 的 bin 加进 PATH，或用 windeployqt 拷一份
set PATH=C:\Qt\5.15.2\msvc2019_64\bin;%PATH%
bin\ens_app.exe
```

✅ **验证（全过才算 BUILD-0 通过）**：
1. 窗口正常弹出，标题 `EnerSentry`，中央显示 `EnerSentry boot OK — BUILD-0 passed`；
2. **不报「由于找不到 Qt5Core.dll / Qt5Widgets.dll 无法继续执行」** —— 这条过了，说明 SHARED 动态链接 + DLL 部署路径正确（否则后面每个 exe 都卡这）；
3. 关闭窗口，进程干净退出，无崩溃栈。

> **为什么必须过这一关才编码**：它把「环境」和「代码」彻底解耦。过了 BUILD-0，之后任何编译/链接/运行错误，你都能确定是**自己业务代码**的问题，而不是「Qt 没配对」「AUTOMOC 没开」「DLL 没部署」这类环境问题——定位成本从「玄学」降为「具体问题」。

---

## 7. 然后才正式编码（接 `ENS-DEV-GUIDE`）

BUILD-0 绿了，按下面顺序接手业务（详细步骤/测试/参考文档全在 `ENS-DEV-GUIDE`）：

1. **测试地基**（DEV-GUIDE §1.4 Step 0）：落 Catch2、`tests/` 目录、`qRegisterMetaType` 集中注册、统一日志规范。
2. **自底向上实现主程序**：L1 通信接入 → L2 协议引擎 → L3 数据中枢 → L4 业务逻辑（AuthManager/SessionManager/SBO）→ L5 UI → `app` 装配。每层配 Tier 2 单测。
3. **测试台作为对端，从 B0 就并行起最小从站**：`ENS-DEV-GUIDE` 的「双轨锁步」要求——主程序写完 `TcpChannel`（Phase 1）的同一阶段，测试台必须写完最小 TCP 从站（B3），否则没法联调。**不要等主程序全写完再造对端**，那是联调灾难。
4. **每 Phase 收口联调**（DEV-GUIDE §2C/§3C/§4C/§5C）：Tier 3 跑通才进下一 Phase。

参考推进节奏（`ENS-DEV-GUIDE` §8）：W1 脚手架+L1 / W2 L2 / W3 L3 / W4 L4 / W5 L5 / W6+ 烤机与 CI。

---

## 8. 编码前最常见的 5 个坑

1. **Qt 编译器与 MSVC 版本不匹配** → 链接报错或一运行就崩。先确认 Qt 包是 `msvc2019_64` 还是 `mingw81_64`，编译器跟着选。
2. **没设 `CMAKE_TOOLCHAIN_FILE` 指向 vcpkg** → `find_package(Qt5/qcustomplot/...)` 全部失败。配置时务必 `-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake`；若不用 vcpkg 才需要 `CMAKE_PREFIX_PATH` 指到 Qt。
3. **`AUTOMOC` 没开** → 带 `Q_OBJECT` 的类链接报 `undefined reference to vtable for Xxx`。确认全局 `CMAKE_AUTOMOC=ON` 或 target 级 `set_target_properties(... PROPERTIES AUTOMOC ON)`。
4. **SHARED Qt 但没部署 DLL** → 双击 exe 报「缺少 Qt5Core.dll」。开发期把 Qt `bin` 加 `PATH`，或 `windeployqt bin/ens_app.exe` 拷贝依赖。
5. **vcpkg triplet 与编译器架构不匹配** → 如用 `x64-windows` 却配 32 位 MSVC/Qt，链接 ABI 错。统一 `x64-windows` + 64 位 MSVC/Qt（`msvc2019_64`）。

---

## 9. 文档衔接：编码前/中/后该查哪份

| 阶段 | 查这份 | 它管什么 |
| --- | --- | --- |
| **编码前（本文）** | `ENS-DEV-BOOT`（本册） | 工具链、依赖来源、CMake 骨架、BUILD-0 冒烟 |
| 建目录时 | `ENS-DEV-ARCH 工程目录架构` | 每个文件/Target/命名空间落在哪、三方库/产物/.ui 放哪、登录与 meta.db 落点 |
| **编码中（逐 Phase）** | `ENS-DEV-GUIDE 上位机开发步骤指南` | 双轨锁步步骤、每步 Tier1/2/3 测试、验证命令、踩坑速查 |
| 设计真相源 | `ENS-HLD-*` / `ENS-LLD-*` / `ENS-(HLD\|LLD\|SIM)-SIM` | 为什么这么写、接口契约、状态机、DB 表 |

> 一句话：**ARCH 告诉你文件放哪 → 本册告诉你怎么把空壳编译运行起来 → GUIDE 告诉你业务怎么一步步写、每步怎么验。**

---

*本文基于 `ENS-DEV-ARCH` V（工程目录架构）、`ENS-DEV-GUIDE` V4.0（双轨开发步骤）及 `ENS-HLD/LLD/SIM` 全套设计汇总编制；依赖统一经 vcpkg（仓库根 vcpkg.json manifest + cmake/Ens3rdparty.cmake）引入，严格沿用上游 `ens_3rdparty` INTERFACE 库收口约定。*
