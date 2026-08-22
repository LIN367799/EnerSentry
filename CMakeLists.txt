cmake_minimum_required(VERSION 3.21)
project(EnerSentry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 子目录可通过 include(<name>) 解析 cmake/ 下的辅助模块
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Qt 运行时部署（windeployqt），见 cmake/EnsDeploy.cmake
include(EnsDeploy)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# 按配置分目录输出，避免 Debug/Release 的 exe 与 Qt DLL 互相覆盖（ENS-DEV-ARCH §2.2.2）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>)

include(CTest)

# 第三方依赖唯一收口，详见 cmake/Ens3rdparty.cmake
include(cmake/Ens3rdparty.cmake)

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
