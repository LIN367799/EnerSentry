# 上位机主程序（被测系统）
add_executable(ens_app
    src/app/main.cpp)
target_link_libraries(ens_app PRIVATE ens::3rdparty)

ens_windeployqt(ens_app)
