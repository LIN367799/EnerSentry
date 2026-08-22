# QCustomPlot 2.1.1 源码 vendored，不经 vcpkg（避免拉起 qtbase 重复编译）
# 来源：D:\HJL\qt\QCustomPlot\qcustomplot\（GPLv3，闭源/商业分发需商业许可）
add_library(qcustomplot STATIC
    qcustomplot.cpp
    qcustomplot.h)

target_include_directories(qcustomplot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qcustomplot PUBLIC Qt5::Widgets Qt5::PrintSupport)

# 抑制 MSVC STL4043 告警（内部使用 stdext::checked_array_iterator）
target_compile_definitions(qcustomplot PRIVATE _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)

add_library(qcustomplot::qcustomplot ALIAS qcustomplot)
