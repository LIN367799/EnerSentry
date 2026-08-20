# cmake/Ens3rdparty.cmake
# 第三方依赖唯一收口点（ENS-DEV-ARCH §1 / ENS-DEV-BOOT §4）
#
# 依赖引入方式（以 ENS-DEV-ENV §1 最终决策为准，区别于 ENS-DEV-BOOT 的纯 vcpkg 写法）：
#   - Qt5 (Core/Gui/Widgets/PrintSupport/SerialPort/Network/Sql) ：复用独立套件，由 CMAKE_PREFIX_PATH
#       指向 D:\HJL\qt\5.15.2\msvc2019_64 解析（配置时由根 CMakeLists 传入）。不经 vcpkg 编译。
#   - Catch2 / nlohmann_json / spdlog            ：经 vcpkg（CMAKE_TOOLCHAIN_FILE）注入。
#   - QCustomPlot 2.1.1                          ：源码 vendored 进 3rdparty/qcustomplot/，
#       不经 vcpkg（避免 qcustomplot 端口拉起 qtbase 重复编译 / 双 Qt 运行时冲突）。
#
# 业务代码永远只 target_link_libraries(... PRIVATE ens::3rdparty)，不关心依赖细节。

add_library(ens_3rdparty INTERFACE)
add_library(ens::3rdparty ALIAS ens_3rdparty)

# —— Qt5（独立套件，CMAKE_PREFIX_PATH 解析）——
find_package(Qt5 COMPONENTS Core Gui Widgets PrintSupport SerialPort Network Sql REQUIRED)

# —— 轻量库（vcpkg 提供，CMAKE_TOOLCHAIN_FILE 注入查找路径）——
find_package(Catch2 CONFIG REQUIRED)        # 目标 Catch2::Catch2WithMain
find_package(nlohmann_json CONFIG REQUIRED) # 目标 nlohmann_json::nlohmann_json
find_package(spdlog CONFIG REQUIRED)        # 目标 spdlog::spdlog

# —— QCustomPlot 源码 vendored：先于 link 定义目标（需要 Qt5::Widgets/PrintSupport，已上一步 find 好）——
add_subdirectory(${CMAKE_SOURCE_DIR}/3rdparty/qcustomplot)

# —— 统一收口 ——
target_link_libraries(ens_3rdparty INTERFACE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::SerialPort
    Qt5::Network
    Qt5::Sql
    qcustomplot::qcustomplot
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    spdlog::spdlog)

# 头文件搜索路径透传（SHARED 模块导出宏等）
target_include_directories(ens_3rdparty INTERFACE
    ${CMAKE_SOURCE_DIR}/apps/ens_app/include
    ${CMAKE_SOURCE_DIR}/apps/device_simulator/include)
