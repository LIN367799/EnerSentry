# ens_tests：Catch2 单测入口
# Step 0 测试地基（ENS-DEV-GUIDE §1.4）：在写第一行业务代码前铺好，后续每步即写即验。
# B1 起编译测试台 src/core 源文件（crc16/mbap，自包含拷贝，不共享编译单元）：
# loopback 用例实证"测试台 CRC 方言 == 主程序 CRC 方言"（§2C CRC 方言行）。
# B3 起追加 src/sim（从站 + TCP server），Tier 3 在测试进程内起真实 socket 通路（无需外部工具）。
add_executable(ens_tests
    unit/test_sanity.cpp
    unit/test_txid_allocator.cpp
    unit/test_accumulator.cpp
    unit/test_crc16.cpp
    unit/test_mbap.cpp
    unit/test_modbus_slave.cpp
    integration/test_modbus_loopback.cpp
    integration/test_modbus_tcp_loopback.cpp
    ../apps/device_simulator/src/core/crc16.cpp
    ../apps/device_simulator/src/core/mbap.cpp
    ../apps/device_simulator/src/sim/modbus_slave.cpp
    ../apps/device_simulator/src/sim/modbus_tcp_server.cpp)
# ens::protocol 透传 channel/3rdparty（Catch2 + Qt 头经此注入）；core/sim 头经 device_simulator/src 解析
target_include_directories(ens_tests PRIVATE
    ../apps/device_simulator/src)
target_link_libraries(ens_tests PRIVATE ens::3rdparty ens::protocol)
# B3 起原生 socket（winsock2）：MSVC 下需显式链 ws2_32（Qt5::Network 也带，双保险）
if(WIN32)
    target_link_libraries(ens_tests PRIVATE ws2_32)
endif()

include(Catch)
catch_discover_tests(ens_tests)
