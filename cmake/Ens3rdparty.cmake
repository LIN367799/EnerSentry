# 第三方依赖唯一收口点（ENS-DEV-ARCH §1）
# 引入方式：Qt5 走独立套件（CMAKE_PREFIX_PATH，不经 vcpkg）；Catch2/nlohmann_json/spdlog 走 vcpkg；
# QCustomPlot 源码 vendored 进 3rdparty/（避免拉起 qtbase 重复编译 / 双 Qt 运行时冲突）。
# 业务代码只 target_link_libraries(... PRIVATE ens::3rdparty)，不关心依赖细节。

add_library(ens_3rdparty INTERFACE)
add_library(ens::3rdparty ALIAS ens_3rdparty)

# Qt5：独立套件，由 CMAKE_PREFIX_PATH 解析
find_package(Qt5 COMPONENTS Core Gui Widgets PrintSupport SerialPort Network Sql REQUIRED)

# 轻量库：vcpkg 经 CMAKE_TOOLCHAIN_FILE 注入查找路径
find_package(Catch2 CONFIG REQUIRED)        # Catch2::Catch2WithMain
find_package(nlohmann_json CONFIG REQUIRED) # nlohmann_json::nlohmann_json
find_package(spdlog CONFIG REQUIRED)        # spdlog::spdlog

# QCustomPlot 源码 vendored
add_subdirectory(${CMAKE_SOURCE_DIR}/3rdparty/qcustomplot)

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
