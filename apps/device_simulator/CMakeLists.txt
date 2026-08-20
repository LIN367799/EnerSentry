# 测试台（对端）—— BUILD-0 阶段先只链 Qt + 空 GUI 壳
# 后续（ENS-DEV-ARCH §2.1.1）：add_library(sim_engine STATIC ...) 纯 C++17 零 Qt
# + add_library(sim_gui ...) + 再链入 DeviceSimulator
add_executable(DeviceSimulator
    src/main_gui.cpp)          # ← BUILD-0 阶段只有 GUI 空壳
target_link_libraries(DeviceSimulator PRIVATE ens::3rdparty)

include(EnsDeploy)
ens_windeployqt(DeviceSimulator)
