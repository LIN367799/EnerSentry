# ENS-DEV-ENV 项目环境与工具链版本规格

> **单一事实来源（Single Source of Truth）**：本文档记录构建 EnerSentry 所需的全部工具、依赖的**精确版本、类型、安装路径、用途与既定配置**。环境搭建、CI 配置、新成员上手均以本文为准。
>
> 探测时间：2026-08-20 20:46 (GMT+8) ｜ 探测环境：Windows 11 (win32)
> 关联文档：ENS-DEV-ARCH（目录架构）、ENS-DEV-BOOT（启动骨架）、ENS-DEV-GUIDE（开发步骤）

---

## 1. 既定配置决策（Decision Summary）

下列选择项已与用户确认，作为本项目的固定配置：

| 决策项 | 选定方案 | 说明 |
|---|---|---|
| 主编译器 | **MSVC v143**（VS2022 工具集 v14.44.35207） | 符合 ENS-DEV-BOOT 钉死方案，ABI 兼容 Qt `msvc2019_64` |
| 对应 Qt 套件 | **Qt 5.15.2 `msvc2019_64`** | 与 MSVC 主链匹配 |
| Qt 引入方式 | **复用独立 Qt**（不经 vcpkg 编译） | 通过 `CMAKE_PREFIX_PATH` 指向 `D:\HJL\qt\5.15.2\msvc2019_64`；Catch2 / nlohmann_json / spdlog 经 vcpkg；QCustomPlot 走源码 vendored |
| vcpkg triplet | **x64-windows（动态链接）** | 与 Qt LGPL / SHARED 一致 |
| CMake 生成器 | **Ninja** | 文档推荐，增量编译快，兼容 MSVC / MinGW |

---

## 2. 工具清单（已查实）

### 2.1 编译器

| 工具 | 类型 | 版本 | 安装路径 | 用途 | 状态 |
|---|---|---|---|---|---|
| MSVC (`cl.exe`) | C++ 编译器 | VS2022 **Community 17.14.36908.2**，工具集 **v14.44.35207 (v143)** | `D:\Program Files\VS2022\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe` | **主编译器** | ✅ |
| MinGW-w64 GCC | C++ 编译器 | **14.2.0** (x86_64-posix-seh-rev1) | `D:\Program Files\mingw64\bin\` | 备选编译器（未选用） | ✅ 在场，非主链 |

> ⚠️ VS2022 装在 **D 盘**而非默认 C 盘；普通终端不加载 MSVC 环境，需用「VS2022 开发人员命令提示符」或 `vcvars64.bat` 加载后再跑 CMake / Ninja。

### 2.2 构建系统 / 生成器

| 工具 | 类型 | 版本 | 安装路径 | 用途 |
|---|---|---|---|---|
| CMake | 构建系统 | **4.1.1** | `D:\HJL\cmake\bin\cmake` | 项目构建编排（≥3.21 ✓） |
| Ninja | 构建生成器 | **1.12.0** | `D:\Tool\perl\c\bin\ninja` | 实际执行编译（文档推荐） |

### 2.3 Qt 框架

| 工具 | 类型 | 版本 | 安装路径 | 用途 | 状态 |
|---|---|---|---|---|---|
| Qt 5.15.2 (LTS) | 应用框架 | **5.15.2** | `D:\HJL\qt\5.15.2\` | **主体 UI / 事件循环 / 信号槽 / SQL** | ✅ 主用 `msvc2019_64` |
| └ `msvc2019_64` 套件 | Qt 构建套件 | 5.15.2 | `D:\HJL\qt\5.15.2\msvc2019_64\` | 配合 MSVC 主链 | ✅ 选用 |
| └ `mingw81_64` 套件 | Qt 构建套件 | 5.15.2 | `D:\HJL\qt\5.15.2\mingw81_64\` | 配合 MinGW（未选用） | 在场，非主链 |
| └ `msvc2019` / `mingw81_32` / `Src` | 其它套件 | 5.15.2 | `D:\HJL\qt\5.15.2\` | — | 备用 |
| Qt 6.11.1 | 应用框架 | 6.11.1 | `D:\HJL\qt\6.11.1\` | — | ❌ 本项目不用 |
| Anaconda 自带 Qt | 应用框架 | 5.15.2 | `D:\HJL\anaconda\Library\` | 仅供 Python（PySide/PyQt） | ❌ 本项目不用 |
| QCustomPlot | 图表库（源码） | **2.1.1 (GPL)** | `D:\HJL\qt\QCustomPlot\qcustomplot\` | 曲线 / 实时绘图，将 vendored 进 `3rdparty/` | ✅ |

### 2.4 包管理器

| 工具 | 类型 | 版本 | 安装路径 | 用途 |
|---|---|---|---|---|
| vcpkg | C++ 包管理器 | **2026-03-04** | `D:\Tool\vcpkg\` | 引入 Catch2 / nlohmann_json / spdlog（triplet x64-windows） |
| └ 工具链文件 | — | — | `D:\Tool\vcpkg\scripts\buildsystems\vcpkg.cmake` | CMake 接入点（`CMAKE_TOOLCHAIN_FILE`） |

> vcpkg 当前 `installed/x64-windows` 已装 Catch2 / nlohmann_json / spdlog（及其传递依赖 fmt）。OpenCV 系依赖（abseil / flatbuffers / opencv4 / protobuf / hiredis / libpng / libjpeg / webp / tiff / zlib 等，系历史遗留）已于 2026-08-20 清理。QCustomPlot 不进 vcpkg，走源码 vendored（见 §2.3 / §3）。

### 2.5 SDK / 运行时 / 语言标准

| 项 | 类型 | 版本 | 路径 | 说明 |
|---|---|---|---|---|
| Windows SDK | 平台 SDK | **10.0.26100.0** | `C:\Program Files (x86)\Windows Kits\10\` | 桌面 C++ 开发必需 |
| C++ 标准 | 语言标准 | **C++17** | — | 文档钉死，不可擅自升级 |
| MSVC CRT | 运行时 | v143 对应 | — | 动态链接（`/MD`） |

### 2.6 版本控制 / 调试

| 工具 | 类型 | 版本 | 路径 | 说明 |
|---|---|---|---|---|
| Git | 版本控制 | **2.55.0.windows.3** | (PATH) | 仓库管理 |
| CDB (Windows Debugger) | 调试器 | 随 Windows SDK | — | MSVC 链配套调试器 |
| GDB | 调试器 | 随 MinGW 14.2.0 | `D:\Program Files\mingw64\bin\` | MinGW 链配套（未选用） |

---

## 3. 依赖清单（按引入方式分组）

| 依赖 | 版本 | 类型 / 许可 | 引入方式 | 配置要点 |
|---|---|---|---|---|
| Qt5 (Core/Gui/Widgets/PrintSupport/SerialPort/Network/Sql) | 5.15.2 | 框架 / LGPL | 独立套件 + `CMAKE_PREFIX_PATH` | `Qt5_DIR=D:\HJL\qt\5.15.2\msvc2019_64\lib\cmake\Qt5`（SerialPort 用于 Modbus RTU/TCP 从站；PrintSupport 用于 QCustomPlot PDF 导出） |
| QCustomPlot | 2.1.1 | 图表库 / GPLv3 | 源码 vendored 进 `3rdparty/` | 随工程编译，链接其 `qcustomplot.cpp/.h`；注意 GPL 许可（见 §5） |
| Catch2 | （待装） | 单测框架 / BSL-1.0 | vcpkg (x64-windows) | `vcpkg install catch2` |
| nlohmann_json | （待装） | JSON 库 / MIT | vcpkg (x64-windows) | `vcpkg install nlohmann-json` |
| spdlog | （待装） | 日志库 / MIT | vcpkg (x64-windows) | `vcpkg install spdlog` |
| SQLite (WAL) | 随 Qt | 嵌入式 DB / 公有领域 | Qt5::Sql 模块 | 经 `Qt5::Sql` 使用，无需单独安装 |

---

## 4. 环境配置命令（可直接复制）

### 4.1 加载 MSVC 环境（任选其一）
- 打开「VS2022 开发人员命令提示符 (x64)」；或
- 在普通终端执行：`call "D:\Program Files\VS2022\VC\Auxiliary\Build\vcvars64.bat"`

### 4.2 配置并构建（Ninja + MSVC + 独立 Qt + vcpkg）

```bat
:: 1) 先安装项目依赖（仅需一次）
D:\Tool\vcpkg\vcpkg.exe install catch2 nlohmann-json spdlog --triplet x64-windows

:: 2) 配置
cmake -G Ninja ^
  -S D:\Study\Qt_host_application_Project\EnerSentry ^
  -B D:\Study\Qt_host_application_Project\EnerSentry\build ^
  -DCMAKE_BUILD_TYPE=Debug ^
  -DCMAKE_PREFIX_PATH="D:\HJL\qt\5.15.2\msvc2019_64" ^
  -DCMAKE_TOOLCHAIN_FILE="D:\Tool\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows

:: 3) 编译
cmake --build D:\Study\Qt_host_application_Project\EnerSentry\build
```

> 说明：`CMAKE_PREFIX_PATH` 让 CMake 在 `D:\HJL\qt\5.15.2\msvc2019_64` 找到 Qt5；`CMAKE_TOOLCHAIN_FILE` 让 vcpkg 注入 Catch2 / nlohmann_json / spdlog。QCustomPlot 不进 vcpkg，作为源码 vendored 进 `3rdparty/` 随工程编译（见 §3）。`build/` 为 out-of-source，gitignore，勿入库。

### 4.3 Agent / 非「VS 开发提示符」环境的一键构建（env-bootstrap）

> 本仓库附 `tools/ens_configure.ps1`：在**无法使用 vcvars64.bat**（如某些拦截 cmd.exe 的沙箱 / Agent 环境）时，手动装配 MSVC + Windows SDK 的 `PATH/INCLUDE/LIB`，并让 CMake **自动从 PATH 发现 rc.exe**。
>
> ⚠️ **不要**手写 `-DCMAKE_RC_COMPILER` 的反斜杠路径——CMake 会把该路径重写进 `CMakeRCCompiler.cmake`，因 `\P` 被当作转义符而报 `Invalid character escape '\P'` 错误。让 CMake 自己从 PATH 探测 rc.exe（路径会被规范成正斜杠）即可。
>
> 用法（普通 PowerShell）：
> ```powershell
> powershell -ExecutionPolicy Bypass -File tools/ens_configure.ps1
> # 可选参数：-BuildType Release / -NoTest
> ```
> 脚本自动探测 VS2022 的 MSVC 版本目录与 Windows SDK（10.0.26100.0），无需硬编码版本号。BUILD-0 已用此脚本实机验证通过（configure / build / test 全绿，CTest 1/1 Passed）。

---

## 5. 许可注意（License Caveats）

- **Qt 5.15.2**：LGPLv3 —— 必须**动态链接** Qt（不静态链 Qt 库），分发时需随附 Qt 动态库与 LGPL 文本。当前 x64-windows 动态方案合规。
- **QCustomPlot 2.1.1**（源码 vendored，GPLv3 或商业许可）：若 EnerSentry 为闭源 / 商业分发，**需购买 QCustomPlot 商业许可**或将项目开源，否则存在许可风险。文档层面已决定以源码 vendored 形式引入 `3rdparty/`，落地前请确认授权。
- **Catch2 (BSL-1.0) / nlohmann_json (MIT) / spdlog (MIT)**：宽松许可，可自由使用。

---

## 6. 待办 / 缺口（Gap）

- [x] `vcpkg install catch2 nlohmann-json spdlog --triplet x64-windows`（已完成）
- [x] 将 `D:\HJL\qt\QCustomPlot\qcustomplot\` 源码 vendored 进 `EnerSentry/3rdparty/`（QCustomPlot 2.1.1 走源码，不经 vcpkg；`3rdparty/qcustomplot/CMakeLists.txt` 已加 STL4043 告警抑制）
- [x] 编写构建骨架：根 `CMakeLists.txt` / `vcpkg.json` / `cmake/Ens3rdparty.cmake` + 三子工程 `CMakeLists.txt` + `main.cpp` 空壳（BUILD-0 冒烟）
- [x] BUILD-0 实机验证通过：configure / build 全绿（16/16 步），三产物 `bin/ens_app.exe` / `bin/DeviceSimulator.exe` / `bin/ens_tests.exe` 生成，CTest 1/1 Passed（2026-08-20，经 `tools/ens_configure.ps1`）
- [x] 目录骨架已建（见 ENS-DEV-ARCH 落地）
- [x] 工具链版本已查清并定型（本文档）

---

## 7. 探测记录（Provenance）

- 探测时间：2026-08-20 20:46 (GMT+8)
- 探测方式：本地文件系统枚举 + 各工具 `--version` / `qmake --version` / `vswhere` / vcpkg `installed/` 清单
- 已实机校验：`vcpkg.cmake` 工具链文件存在、`Qt5` CMake 配置目录存在、Catch2 / nlohmann_json / spdlog 三个 vcpkg 依赖缺失状态。
- VS 版本由 `vswhere -all -format xml` 取 `installationVersion=17.14.36908.2`、`displayName=Visual Studio Community 2022`。
- 修订（2026-08-20 21:24）：QCustomPlot 改为经 vcpkg 引入（port 2.1.1#1，GPL-3.0-or-later），并新增 §5.1 说明 qcustomplot 端口依赖 qtbase 导致的 Qt5 重复编译/双运行时冲突，以及 overlay 端口处理方案；同步更新 §1/§2.3/§2.4/§3/§4.2/§5/§6。vcpkg 已实装 Catch2 / nlohmann_json / spdlog（含传递依赖 fmt），OpenCV 系历史依赖已清理。
- 修订（2026-08-20 21:29）：QCustomPlot 决策**回退**为走源码 vendored（不经 vcpkg）。撤销 §5.1 及所有 qcustomplot-overlay 相关表述；§2.3 来源改回 `D:\HJL\qt\QCustomPlot\qcustomplot\` 源码，§3 引入方式改回「源码 vendored 进 3rdparty/」，§4.2 安装命令移除 qcustomplot，§6 待办改回「vendored 进 3rdparty/」。vcpkg 职责收窄为仅 Catch2 / nlohmann_json / spdlog。
