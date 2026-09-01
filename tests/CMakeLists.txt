# ens_tests：Catch2 单测入口
# Step 0 测试地基（ENS-DEV-GUIDE §1.4）：在写第一行业务代码前铺好，后续每步即写即验。
# B1 起编译测试台 src/core 源文件（crc16/mbap，自包含拷贝，不共享编译单元）：
# loopback 用例实证"测试台 CRC 方言 == 主程序 CRC 方言"（§2C CRC 方言行）。
# B3 起追加 src/sim（从站 + TCP server），Tier 3 在测试进程内起真实 socket 通路（无需外部工具）。
add_executable(ens_tests
    main.cpp
    unit/test_sanity.cpp
    unit/test_txid_allocator.cpp
    unit/test_accumulator.cpp
    unit/test_crc16.cpp
    unit/test_crc16_master.cpp
    unit/test_mbap.cpp
    unit/test_modbus_slave.cpp
    unit/test_modbus_frame.cpp
    unit/test_modbus_engine.cpp
    unit/test_pointtable.cpp
    unit/test_point_generator.cpp
    unit/test_poll_scheduler.cpp
    unit/test_register_bank.cpp
    unit/test_sample.cpp
    unit/test_ringbuffer.cpp
    unit/test_l1snapshot.cpp
    unit/test_databus.cpp
    unit/test_down_sampler.cpp
    unit/test_sqlite_data_access.cpp
    unit/test_history_query.cpp
    unit/test_l2history_store.cpp
    unit/test_platform_mmap.cpp
    unit/test_blackbox.cpp
    unit/test_business_state_machine.cpp
    unit/test_alarm_engine.cpp
    unit/test_alarm_direction.cpp
    unit/test_auth_manager.cpp
    unit/test_auth_security.cpp
    unit/test_rbac.cpp
    unit/test_render_downsampler.cpp
    unit/test_sbo_state_machine.cpp
    unit/test_business_integration.cpp
    unit/test_fault_injector.cpp
    unit/test_simulator_engine.cpp
    unit/test_scenario_script.cpp
    unit/test_alarm_word_io.cpp
    unit/test_alarm_rule_loader.cpp
    integration/test_modbus_loopback.cpp
    integration/test_modbus_tcp_loopback.cpp
    integration/test_modbus_engine_loopback.cpp
    integration/test_modbus_slave_emulator.cpp
    integration/test_tcp_channel.cpp
    integration/test_p1_closeout.cpp
    integration/test_serial_channel.cpp
    integration/test_main_loop.cpp
    ../apps/device_simulator/src/core/crc16.cpp
    ../apps/device_simulator/src/core/mbap.cpp
    ../apps/device_simulator/src/core/point_table.cpp
    ../apps/device_simulator/src/sim/register_bank.cpp
    ../apps/device_simulator/src/sim/sim_config.cpp
    ../apps/device_simulator/src/sim/point_generator.cpp
    ../apps/device_simulator/src/sim/FaultInjector.cpp
    ../apps/device_simulator/src/sim/SimulatorEngine.cpp
    ../apps/device_simulator/src/sim/scenario_script.cpp
    ../apps/device_simulator/src/sim/modbus_dispatch.cpp
    ../apps/device_simulator/src/sim/modbus_slave_emulator.cpp
    ../apps/device_simulator/src/sim/modbus_slave.cpp
    ../apps/device_simulator/src/sim/modbus_tcp_server.cpp
    ../apps/device_simulator/src/sim/rtu_slave_port.h
    ../apps/device_simulator/src/sim/rtu_slave_port.cpp
    # 切片 14: 主程序接线器编译进测试进程（与 sim 文件同模式,显式列源）
    ../apps/ens_app/src/app/EnerSentryApp.cpp)
# ens::protocol 透传 channel/3rdparty（Catch2 + Qt 头经此注入）；core/sim 头经 device_simulator/src 解析
# 主程序协议侧（apps/ens_app/src/{protocol,datahub}）通过 datahub/Sample.h + protocol/{Crc16,ModbusFrame,PointTable}.h 取值
target_include_directories(ens_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../apps/device_simulator/src
    ../apps/ens_app/src)
target_link_libraries(ens_tests PRIVATE ens::3rdparty ens::protocol ens::datahub ens::business ens::ui Catch2::Catch2)
# B3 起原生 socket（winsock2）：用 find_library 给完整路径 + 显式 LIBPATH(避免依赖
#  PowerShell Enter-VsDevShell 的 LIB env 在 ninja→cmd spawn 链中丢失)
if(WIN32)
    find_library(WS2_32_LIB NAMES ws2_32 PATHS
        "C:/Program Files (x86)/Windows Kits/10/Lib"
        "C:/Program Files/Windows Kits/10/Lib"
        REQUIRED)
    target_link_libraries(ens_tests PRIVATE "${WS2_32_LIB}")
    # 显式加 Windows SDK + MSVC lib 路径,供 link 找 kernel32/user32/... 等系统库
    target_link_options(ens_tests PRIVATE
        "LINKER:/LIBPATH:C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/um/x64"
        "LINKER:/LIBPATH:C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/ucrt/x64"
        "LINKER:/LIBPATH:D:/Program Files/VS2022/VC/Tools/MSVC/14.44.35207/lib/x64")
endif()

# 把 sim_pointtable_sample.json 拷贝到 ens_tests 输出目录的 test_data/ 下。
# 这样 test_pointtable.cpp 可以用相对路径 "test_data/sim_pointtable_sample.json" 加载,
# 避免 std::filesystem::path + 中文路径 + MSVC system code page 的三重坑:
#   1) CMake target_compile_definitions 注入含中文路径 → MSVC 字符串字面量截断
#   2) std::filesystem::path::string()/wstring() → system_error "No mapping for Unicode..."
#
# 用 file(COPY ...) 在 configure 期执行(非 POST_BUILD 命令),避免 cmd.exe 把含"测试台"
# 的源路径按 GBK 编码后识别为 `"` 终结符,导致 CatchAddTests.cmake 解析 ens_tests-b12d07c
# _tests.cmake 失败。file(COPY) 走 CMake 内部 API,路径 UTF-16 直传,完全绕开 shell 编码层。
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_data)
file(COPY "${CMAKE_SOURCE_DIR}/docs/04-测试台/data/sim_pointtable_sample.json"
     DESTINATION ${CMAKE_BINARY_DIR}/test_data)
# 切片 15 B9：drill 场景脚本复制到 test_data/scenarios/（test_scenario_script.cpp 用相对路径加载）
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_data/scenarios)
file(COPY
     "${CMAKE_SOURCE_DIR}/docs/04-测试台/scenarios/overheat_drill.json"
     "${CMAKE_SOURCE_DIR}/docs/04-测试台/scenarios/overheat_fast.json"
     "${CMAKE_SOURCE_DIR}/docs/04-测试台/scenarios/random_linkloss_stress.json"
     "${CMAKE_SOURCE_DIR}/docs/04-测试台/scenarios/voltage_fault_drill.json"
     DESTINATION ${CMAKE_BINARY_DIR}/test_data/scenarios)
# 切片 16：告警规则 sample 复制到 test_data/（test_alarm_rule_loader / test_main_loop 用）
file(COPY "${CMAKE_SOURCE_DIR}/docs/04-测试台/data/alarm_rules_sample.json"
     DESTINATION ${CMAKE_BINARY_DIR}/test_data)

# Qt SQL driver:Debug 模式需要 qsqlited.dll 在 plugins/sqldrivers/ 子目录
# (在 CMake configure 时同步复制,避免手工操作;依赖 Qt 5.15 默认布局)
set(_QT_SQL_PLUGIN_DIR "D:/HJL/qt/5.15.2/msvc2019_64/plugins/sqldrivers")
if(EXISTS "${_QT_SQL_PLUGIN_DIR}/qsqlited.dll")
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/bin/Debug/plugins/sqldrivers)
    file(COPY "${_QT_SQL_PLUGIN_DIR}/qsqlited.dll"
         DESTINATION ${CMAKE_BINARY_DIR}/bin/Debug/plugins/sqldrivers)
endif()

# 2026-08-30 修复 ens_tests.exe 桌面双击运行报"找不到 Qt5Sqld.dll":
#   根因:ens_tests 链接 static ens::datahub → 内联 SQLiteDataAccess.cpp → PE
#   导入表直接含 Qt5Sql,运行时需 Qt5Sqld.dll。ctest 跑测时通过 PATH 找 Qt bin
#   目录能通,但桌面双击/资源管理器启动无 PATH,缺主 DLL 即报系统错。
#   ens_windeployqt(ens_tests) 在 POST_BUILD 调 windeployqt,把 Qt5Sqld.dll +
#   plugins/sqldrivers/qsqlited.dll 自动部署到 $<TARGET_FILE_DIR:ens_tests>
#   (= ${CMAKE_RUNTIME_OUTPUT_DIRECTORY} = bin/$<CONFIG>/),与 ens_app 同等待遇。
#   顺带:上方 file(COPY) 路径写的是 ${CMAKE_BINARY_DIR}/bin/Debug/... (即 build/
#   下),与 CMAKE_RUNTIME_OUTPUT_DIRECTORY (项目根 bin/) 错位,目前靠 ens_windeployqt
#   (ens_app) 部署到正确位置掩盖,后续切片统一收口(用 CMAKE_RUNTIME_OUTPUT_DIRECTORY
#   替代 CMAKE_BINARY_DIR/bin/Debug)。
if(WIN32)
    ens_windeployqt(ens_tests)
endif()

include(Catch)
catch_discover_tests(ens_tests)
