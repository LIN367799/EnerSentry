# 上位机主程序（被测系统）—— BUILD-0 阶段仅空壳窗口
# 后续按 ENS-DEV-ARCH 把 src/channel src/protocol src/datahub src/business src/ui 加进各 target
add_executable(ens_app
    src/app/main.cpp)          # ← BUILD-0 阶段只有这一个文件
target_link_libraries(ens_app PRIVATE ens::3rdparty)

include(EnsDeploy)
ens_windeployqt(ens_app)
