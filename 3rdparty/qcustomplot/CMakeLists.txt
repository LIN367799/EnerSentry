# QCustomPlot 2.1.1 源码 vendored（ENS-DEV-ENV §2.3 / §3：走源码，不经 vcpkg）
# 原始来源：D:\HJL\qt\QCustomPlot\qcustomplot\（GPLv3 —— 闭源/商业分发需商业许可）
# 仅取 qcustomplot.cpp / qcustomplot.h，文档与示例不入库。

add_library(qcustomplot STATIC
    qcustomplot.cpp
    qcustomplot.h)

target_include_directories(qcustomplot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qcustomplot PUBLIC Qt5::Widgets Qt5::PrintSupport)

# 抑制 MSVC STL4043 弃用告警：qcustomplot 内部使用 stdext::checked_array_iterator，
# 仅告警、不影响编译；否则每次编译 qcustomplot.cpp 都会刷屏（BUILD-0 验证已确认）。
target_compile_definitions(qcustomplot PRIVATE _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)

add_library(qcustomplot::qcustomplot ALIAS qcustomplot)
