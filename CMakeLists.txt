cmake_minimum_required(VERSION 3.21)
project(EnerSentry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 让子目录可以通过 include(<name>) 找到 cmake/ 下的辅助模块
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# 部署辅助（ens_windeployqt / ens_finalize_deploy）
include(EnsDeploy)

# Qt 自动处理 moc / uic / qrc（ENS-DEV-ARCH §2.2）
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# 最终 exe / DLL 按配置分目录输出：仓库根 bin/<CONFIG>/（ENS-DEV-ARCH §2.2.2）
# 用 $<CONFIG> 区分 Debug / Release，避免两种配置的 exe 与 Qt DLL 互相覆盖/混用
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>)

enable_testing()
include(CTest)

# 依赖唯一收口（cmake/Ens3rdparty.cmake）
# 该文件内部：
#   - find_package(Qt5 ...)        —— Qt5 由 CMAKE_PREFIX_PATH 指向独立套件解析（ENS-DEV-ENV §1）
#   - find_package(Catch2/nlohmann_json/spdlog ...) —— 由 vcpkg（CMAKE_TOOLCHAIN_FILE）注入
#   - add_subdirectory(3rdparty/qcustomplot) —— QCustomPlot 源码 vendored（不经 vcpkg）
include(cmake/Ens3rdparty.cmake)

# 子工程开关（CI 可 -DBUILD_TESTS=OFF 等只测主程序）
option(BUILD_APP        "Build ens_app (主程序)"         ON)
option(BUILD_SIMULATOR "Build device_simulator (测试台)" ON)
option(BUILD_TESTS     "Build ens_tests (单测)"           ON)

if(BUILD_APP)
    add_subdirectory(apps/ens_app)
endif()
if(BUILD_SIMULATOR)
    add_subdirectory(apps/device_simulator)
endif()
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

# 收口：把所有登记的目标合并为【单次串行】的 Qt 运行时部署步骤
# （必须在所有 add_subdirectory 之后调用，见 cmake/EnsDeploy.cmake）
ens_finalize_deploy()
