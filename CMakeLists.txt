cmake_minimum_required(VERSION 3.21)
project(EnerSentry LANGUAGES CXX)

# The supported Windows ABI is MSVC + the Qt msvc2019_64 package.  Check this
# immediately after compiler detection so a MinGW environment fails during
# configure instead of producing opaque Qt linker errors later.
if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR
        "EnerSentry requires the MSVC compiler on Windows. "
        "Start an x64 VS2022 developer shell (or set MSVC_CXX_COMPILER); "
        "MinGW is incompatible with Qt msvc2019_64.")
endif()
if(WIN32 AND MSVC)
    get_filename_component(_ens_compiler_name "${CMAKE_CXX_COMPILER}" NAME)
    if(NOT _ens_compiler_name MATCHES "^cl(\\.exe)?$")
        message(FATAL_ERROR
            "EnerSentry requires cl.exe (MSVC v143) on Windows; "
            "the selected compiler is '${CMAKE_CXX_COMPILER}'.")
    endif()
    if(MSVC_VERSION LESS 1930)
        message(FATAL_ERROR "EnerSentry requires Visual Studio 2022 (MSVC v143 or newer); found MSVC_VERSION=${MSVC_VERSION}.")
    endif()
endif()

set(ENS_EXPECTED_QT_VERSION "5.15.2" CACHE STRING
    "Qt version required by the supported build baseline")
set(ENS_EXPECTED_QT_ABI "msvc2019_64" CACHE STRING
    "Qt package ABI required by the supported Windows build baseline")

# Keep machine-specific SDK locations out of the project.  The configure
# preset supplies ENS_QT_ROOT from QT_ROOT; command-line -D remains supported.
set(ENS_QT_ROOT "" CACHE PATH
    "Qt installation root (for example C:/Qt/5.15.2/msvc2019_64)")
if(NOT ENS_QT_ROOT AND DEFINED ENV{QT_ROOT})
    set(ENS_QT_ROOT "$ENV{QT_ROOT}" CACHE PATH
        "Qt installation root (for example C:/Qt/5.15.2/msvc2019_64)" FORCE)
endif()
if(ENS_QT_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${ENS_QT_ROOT}")
endif()

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

if(WIN32)
    if(NOT Qt5Core_DIR)
        message(FATAL_ERROR "Qt5Core was not found. Set QT_ROOT/ENS_QT_ROOT to a Qt ${ENS_EXPECTED_QT_VERSION} ${ENS_EXPECTED_QT_ABI} installation.")
    endif()
    get_filename_component(_ens_qt_root "${Qt5Core_DIR}/../../../" ABSOLUTE)
    file(TO_CMAKE_PATH "${_ens_qt_root}" _ens_qt_root_normalized)
    string(TOLOWER "${_ens_qt_root_normalized}" _ens_qt_root_lower)
    string(TOLOWER "${ENS_EXPECTED_QT_ABI}" _ens_expected_qt_abi_lower)
    if(NOT _ens_qt_root_lower MATCHES "/${_ens_expected_qt_abi_lower}$")
        message(FATAL_ERROR
            "Qt ABI mismatch: found '${_ens_qt_root_normalized}', expected a ${ENS_EXPECTED_QT_ABI} installation. "
            "Set QT_ROOT/ENS_QT_ROOT explicitly.")
    endif()
    if(NOT Qt5Core_VERSION VERSION_EQUAL ENS_EXPECTED_QT_VERSION)
        message(FATAL_ERROR
            "Qt version mismatch: found ${Qt5Core_VERSION}, expected ${ENS_EXPECTED_QT_VERSION}.")
    endif()
    message(STATUS "EnerSentry toolchain: MSVC ${MSVC_VERSION}, Qt ${Qt5Core_VERSION} (${ENS_EXPECTED_QT_ABI})")
endif()

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
