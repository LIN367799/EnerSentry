# 测试台（对端）—— B0 脚手架 + B1 ens::core 公共契约 + B2 RegisterBank RCU + B3/B4 最小从站（ENS-SIM-IMP §5.1）
# src/core 与 src/sim 纯 C++17（crc16/mbap/register_bank/从站），与主程序方言逐字节一致但自包含拷贝（§2.1）；
# rtu_slave_port 因串口实现走 QSerialPort（与主程序 SerialChannel 同栈），携带 Qt::SerialPort 依赖。
add_executable(DeviceSimulator
    src/main_gui.cpp
    src/core/crc16.cpp
    src/core/mbap.cpp
    src/core/point_table.cpp
    src/sim/register_bank.cpp
    src/sim/sim_config.cpp
    src/sim/point_generator.cpp
    src/sim/FaultInjector.cpp
    src/sim/modbus_dispatch.cpp
    src/sim/modbus_slave_emulator.cpp
    src/sim/modbus_slave.cpp
    src/sim/modbus_tcp_server.cpp
    src/sim/rtu_slave_port.h
    src/sim/rtu_slave_port.cpp)
target_include_directories(DeviceSimulator PRIVATE src)
target_link_libraries(DeviceSimulator PRIVATE ens::3rdparty)

ens_windeployqt(DeviceSimulator)
