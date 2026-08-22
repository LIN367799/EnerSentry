# 测试台（对端）
add_executable(DeviceSimulator
    src/main_gui.cpp)
target_link_libraries(DeviceSimulator PRIVATE ens::3rdparty)

ens_windeployqt(DeviceSimulator)
