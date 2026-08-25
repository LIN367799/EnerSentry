# apps/ens_app/CMakeLists.txt
# EnerSentry 主程序：五层模块化 Target（ENS-DEV-ARCH §2/§4）。
# 依赖边：ens_app(EXE) → ui → business → datahub → protocol → channel。
# 构建类型：channel/business SHARED（热替换 + LGPL 合规），protocol/datahub/ui STATIC（热路径内联）。
# 三方依赖统一经 ens::3rdparty 透传；STATIC 依赖 SHARED 必须 PUBLIC（防 unresolved external）。

# ---------------- 内部导出宏头（ens/export.hpp），仅 SHARED 层需要 ----------------
add_library(ens_export INTERFACE)
add_library(ens::export ALIAS ens_export)
target_include_directories(ens_export INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------- L1 通信接入层（SHARED） ----------------
# Phase 1 L1 2.1.1：IChannel/ChannelConfig/ChannelStats + SerialChannel/TcpChannel 子类骨架。
# Q_OBJECT 头必须列在 sources 让 AUTOMOC 处理（生成 moc_<class>.cpp 注入元对象代码）。
add_library(ens_channel SHARED
    src/channel/IChannel.h
    src/channel/ChannelConfig.h
    src/channel/ChannelStats.h
    src/channel/SerialChannel.h
    src/channel/SerialChannel.cpp
    src/channel/TcpChannel.h
    src/channel/TcpChannel.cpp)
add_library(ens::channel ALIAS ens_channel)
target_include_directories(ens_channel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/channel)
# SHARED 自身编译期需 dllexport（仅 PRIVATE，不向消费者透传）
target_compile_definitions(ens_channel PRIVATE ENS_BUILD_SHARED)
target_link_libraries(ens_channel PUBLIC ens::export ens::3rdparty)
# 运行时 Qt DLL 自足：windeployqt 不递归分析非 Qt DLL（ens_channel.dll）的 Qt 依赖，
# 故本库 POST_BUILD 把实际用到的 Qt 模块 DLL 拷到输出目录（Network=2.1.3 起；SerialPort 预留 2.1.2）。
foreach(_qtmod Network SerialPort)
    if(TARGET Qt5::${_qtmod})
        add_custom_command(TARGET ens_channel POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:Qt5::${_qtmod}>"
                    "$<TARGET_FILE_DIR:ens_channel>/")
    endif()
endforeach()

# ---------------- L2 协议处理层（STATIC） ----------------
# Phase 2 3.1.1/3.1.2：CRC-16/MODBUS 查表 + ModbusFrame 组帧/拆帧（LLD-100 §4.1）。
# Phase 2 3.1.5：PointTable 点表解析器（§4.4）。
# Phase 2 3.1.3：ModbusEngine 协议语义核心（§4.2.2 + 用户截图 V2：Qt signal/slot + moveToThread）。
# Phase 1 L1:  RTU/TCP 字节流累加器（§4.2.1）。
# 后续 3.1.4 PollScheduler 落 .cpp 时继续 append。
add_library(ens_protocol STATIC
    src/protocol/protocol_anchor.cpp
    src/protocol/ModbusStreamAccumulator.cpp
    src/protocol/Crc16.cpp
    src/protocol/ModbusFrame.cpp
    src/protocol/PointTable.cpp
    src/protocol/ModbusEngine.cpp)
add_library(ens::protocol ALIAS ens_protocol)
target_include_directories(ens_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/protocol)
# PRIVATE include channel — ModbusEngine.cpp 实现依赖 IChannel 头但其 API 不应被协议层消费者看到
target_include_directories(ens_protocol PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/channel)
target_link_libraries(ens_protocol PUBLIC ens::channel ens::3rdparty)

# ---------------- L3 数据中枢层（STATIC） ----------------
add_library(ens_datahub STATIC
    src/datahub/datahub_anchor.cpp)
add_library(ens::datahub ALIAS ens_datahub)
target_include_directories(ens_datahub PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/datahub)
target_link_libraries(ens_datahub PUBLIC ens::protocol ens::3rdparty)

# ---------------- L4 业务逻辑层（SHARED） ----------------
add_library(ens_business SHARED
    src/business/business_anchor.cpp)
add_library(ens::business ALIAS ens_business)
target_include_directories(ens_business PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/business)
target_compile_definitions(ens_business PRIVATE ENS_BUILD_SHARED)
target_link_libraries(ens_business PUBLIC ens::datahub ens::export ens::3rdparty)

# ---------------- L5 UI 视图层（STATIC） ----------------
add_library(ens_ui STATIC
    src/ui/ui_anchor.cpp)
add_library(ens::ui ALIAS ens_ui)
target_include_directories(ens_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
target_link_libraries(ens_ui PUBLIC ens::business ens::3rdparty)

# ---------------- 应用基础设施（EXECUTABLE） ----------------
add_executable(ens_app
    src/app/main.cpp)
# 链接顶层 ui，依赖经 PUBLIC 链自动收敛到 channel；3rdparty 直链保证 main.cpp 的 Qt Widgets 符号可达
target_link_libraries(ens_app PRIVATE ens::ui ens::3rdparty)

ens_windeployqt(ens_app)
