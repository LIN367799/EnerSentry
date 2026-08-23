# apps/ens_app/CMakeLists.txt
# EnerSentry 上位机主程序 —— 五层模块化 Target 拓扑（依据 ENS-DEV-ARCH §2 / §4）
#
# 依赖收敛方向（上层依赖下层，层间只经抽象接口 / 信号槽）：
#     ens_app(EXE) → ens::ui → ens::business → ens::datahub → ens::protocol → ens::channel
#
# 构建类型（混合构建，HLD §2.6.5 / ENS-DEV-ARCH §4）：
#     ens::channel    SHARED   (channel.dll，因站热替换 + LGPL 合规)
#     ens::protocol   STATIC   (热路径，内联进 exe)
#     ens::datahub    STATIC   (热路径，内联进 exe)
#     ens::business   SHARED   (business.dll，因站热替换 + LGPL 合规)
#     ens::ui         STATIC   (热路径，内联进 exe)
#     ens_app         EXECUTABLE
#
# 所有三方依赖（Qt5 / QCustomPlot / Catch2 / nlohmann_json / spdlog）经 ens::3rdparty INTERFACE 库统一透传。
# 关键约定：STATIC 依赖 SHARED 必须用 PUBLIC，否则 exe 链接报 unresolved external（ENS-DEV-ARCH §4）。

# ---------------- 内部导出宏头（ens/export.hpp），仅 SHARED 层需要 ----------------
add_library(ens_export INTERFACE)
add_library(ens::export ALIAS ens_export)
target_include_directories(ens_export INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------- L1 通信接入层（SHARED） ----------------
add_library(ens_channel SHARED
    src/channel/channel_anchor.cpp)
add_library(ens::channel ALIAS ens_channel)
target_include_directories(ens_channel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/channel)
# SHARED 自身编译期需 dllexport（仅 PRIVATE，不向消费者透传）
target_compile_definitions(ens_channel PRIVATE ENS_BUILD_SHARED)
target_link_libraries(ens_channel PUBLIC ens::export ens::3rdparty)

# ---------------- L2 协议处理层（STATIC） ----------------
add_library(ens_protocol STATIC
    src/protocol/protocol_anchor.cpp)
add_library(ens::protocol ALIAS ens_protocol)
target_include_directories(ens_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/protocol)
target_link_libraries(ens_protocol PUBLIC ens::channel ens::3rdparty)

# ---------------- L3 数据中枢层（STATIC） ----------------
add_library(ens_datahub STATIC
    src/datahub/datahub_anchor.cpp)
add_library(ens::datahub ALIAS ens_datahub)
target_include_directories(ens_datahub PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/datahub)
target_link_libraries(ens_datahub PUBLIC ens::protocol ens::3rdparty)

# ---------------- L4 业务逻辑层（SHARED） ----------------
add_library(ens_business SHARED
    src/business/business_anchor.cpp)
add_library(ens::business ALIAS ens_business)
target_include_directories(ens_business PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/business)
target_compile_definitions(ens_business PRIVATE ENS_BUILD_SHARED)
target_link_libraries(ens_business PUBLIC ens::datahub ens::export ens::3rdparty)

# ---------------- L5 UI 视图层（STATIC） ----------------
add_library(ens_ui STATIC
    src/ui/ui_anchor.cpp)
add_library(ens::ui ALIAS ens_ui)
target_include_directories(ens_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
target_link_libraries(ens_ui PUBLIC ens::business ens::3rdparty)

# ---------------- 应用基础设施（EXECUTABLE） ----------------
add_executable(ens_app
    src/app/main.cpp)
# 链接顶层 ui，依赖经 PUBLIC 链自动收敛到 channel；3rdparty 直链保证 main.cpp 的 Qt Widgets 符号可达
target_link_libraries(ens_app PRIVATE ens::ui ens::3rdparty)

ens_windeployqt(ens_app)
