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
    unit/test_ui_smoke.cpp
    unit/test_csv_writer.cpp
    unit/test_auth_security.cpp
    unit/test_user_manage.cpp
    unit/test_must_change.cpp
    unit/test_rbac.cpp
    unit/test_render_downsampler.cpp
    unit/test_sbo_state_machine.cpp
    unit/test_business_integration.cpp
    unit/test_fault_injector.cpp
    unit/test_simulator_engine.cpp
    unit/test_scenario_script.cpp
    unit/test_alarm_word_io.cpp
    unit/test_alarm_rule_loader.cpp
    unit/test_alarm_record_store.cpp
    unit/test_alarm_query.cpp
    unit/test_alarm_notifier.cpp
    unit/test_replay.cpp
    unit/test_realtime_interact.cpp
    unit/test_overview_gauges.cpp
    unit/test_export_utils.cpp
    unit/test_frameless_helper.cpp
    unit/test_titlebar.cpp
    unit/test_chrome_render.cpp
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
    ../apps/ens_app/src/app/EnerSentryApp.cpp
    # 切片 45：UI 图标资源（resources.qrc → :/icons/*.svg）供 FramelessHelper/TitleBar
    # 单测使用；ens_tests 主程序 exe 不链 rcc 时 QIcon 加载 SVG 返 null。
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc)
# ens::protocol 透传 channel/3rdparty（Catch2 + Qt 头经此注入）；core/sim 头经 device_simulator/src 解析
# 主程序协议侧（apps/ens_app/src/{protocol,datahub}）通过 datahub/Sample.h + protocol/{Crc16,ModbusFrame,PointTable}.h 取值
target_include_directories(ens_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../apps/device_simulator/src
    ../apps/ens_app/src)
target_link_libraries(ens_tests PRIVATE ens::3rdparty ens::protocol ens::datahub ens::business ens::ui Catch2::Catch2)
# B3 起原生 socket（winsock2）。CMake's MSVC platform already provides the
# Windows SDK search paths; do not encode a particular SDK or VS installation.
if(WIN32)
    target_link_libraries(ens_tests PRIVATE ws2_32)
endif()

# 把 sim_pointtable_sample.json 拷贝到 ens_tests 输出目录的 test_data/ 下。
# 这样 test_pointtable.cpp 可以用相对路径 "test_data/sim_pointtable_sample.json" 加载,
# 避免 std::filesystem::path + 中文路径 + MSVC system code page 的三重坑:
#   1) CMake target_compile_definitions 注入含中文路径 → MSVC 字符串字面量截断
#   2) std::filesystem::path::string()/wstring() → system_error "No mapping for Unicode..."
#
# 切片 44a：数据工件源迁至仓根 data/（原 docs/04-测试台/data|scenarios，全 ASCII），
# file(COPY) 源路径不再含中文，注释中历史编码坑（GBK `"` 终结符等）自动消失。
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_data)
file(COPY "${CMAKE_SOURCE_DIR}/data/sim_pointtable_sample.json"
     DESTINATION ${CMAKE_BINARY_DIR}/test_data)
# 切片 15 B9：drill 场景脚本复制到 test_data/scenarios/（test_scenario_script.cpp 用相对路径加载）
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_data/scenarios)
file(COPY
     "${CMAKE_SOURCE_DIR}/data/scenarios/overheat_drill.json"
     "${CMAKE_SOURCE_DIR}/data/scenarios/overheat_fast.json"
     "${CMAKE_SOURCE_DIR}/data/scenarios/random_linkloss_stress.json"
     "${CMAKE_SOURCE_DIR}/data/scenarios/voltage_fault_drill.json"
     DESTINATION ${CMAKE_BINARY_DIR}/test_data/scenarios)
# 切片 16：告警规则 sample 复制到 test_data/（test_alarm_rule_loader / test_main_loop 用）
file(COPY "${CMAKE_SOURCE_DIR}/data/alarm_rules_sample.json"
     DESTINATION ${CMAKE_BINARY_DIR}/test_data)

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
