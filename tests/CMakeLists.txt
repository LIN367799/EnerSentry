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
    unit/test_register_bank.cpp
    unit/test_sample.cpp
    integration/test_modbus_loopback.cpp
    integration/test_modbus_tcp_loopback.cpp
    integration/test_tcp_channel.cpp
    integration/test_p1_closeout.cpp
    integration/test_serial_channel.cpp
    ../apps/device_simulator/src/core/crc16.cpp
    ../apps/device_simulator/src/core/mbap.cpp
    ../apps/device_simulator/src/core/point_table.cpp
    ../apps/device_simulator/src/sim/register_bank.cpp
    ../apps/device_simulator/src/sim/sim_config.cpp
    ../apps/device_simulator/src/sim/point_generator.cpp
    ../apps/device_simulator/src/sim/modbus_slave.cpp
    ../apps/device_simulator/src/sim/modbus_tcp_server.cpp
    ../apps/device_simulator/src/sim/rtu_slave_port.h
    ../apps/device_simulator/src/sim/rtu_slave_port.cpp)
# ens::protocol 透传 channel/3rdparty（Catch2 + Qt 头经此注入）；core/sim 头经 device_simulator/src 解析
# 主程序协议侧（apps/ens_app/src/{protocol,datahub}）通过 datahub/Sample.h + protocol/{Crc16,ModbusFrame,PointTable}.h 取值
target_include_directories(ens_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../apps/device_simulator/src
    ../apps/ens_app/src)
target_link_libraries(ens_tests PRIVATE ens::3rdparty ens::protocol Catch2::Catch2)
# B3 起原生 socket（winsock2）：MSVC 下需显式链 ws2_32（Qt5::Network 也带，双保险）
if(WIN32)
    target_link_libraries(ens_tests PRIVATE ws2_32)
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

include(Catch)
catch_discover_tests(ens_tests)
