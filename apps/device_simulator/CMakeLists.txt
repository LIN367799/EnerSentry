# 测试台（对端）—— B0 脚手架 + B1 ens::core 公共契约 + B3 最小 TCP 从站（ENS-SIM-IMP §5.1）
# src/core 与 src/sim 纯 C++17 零 Qt（crc16/mbap/从站），与主程序方言逐字节一致但自包含拷贝（§2.1）。
add_executable(DeviceSimulator
    src/main_gui.cpp
    src/core/crc16.cpp
    src/core/mbap.cpp
    src/sim/modbus_slave.cpp
    src/sim/modbus_tcp_server.cpp)
target_include_directories(DeviceSimulator PRIVATE src)
target_link_libraries(DeviceSimulator PRIVATE ens::3rdparty)

ens_windeployqt(DeviceSimulator)
