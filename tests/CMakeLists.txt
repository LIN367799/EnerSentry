# 三层验证基础设施（ENS-DEV-GUIDE §1.4 / ENS-DEV-ARCH §1）
# BUILD-0 阶段仅挂一个 sanity 用例，验证 Catch2 注册链路 + ens::3rdparty 依赖收口生效。
# 后续按 ENS-DEV-GUIDE 逐步补 unit/test_*.cpp 与 integration/test_modbus_loopback.cpp，
# 并链入 protocol / datahub / sim_engine。
add_executable(ens_tests
    unit/test_sanity.cpp)
target_link_libraries(ens_tests PRIVATE ens::3rdparty)
target_compile_features(ens_tests PRIVATE cxx_std_17)

include(Catch)
catch_discover_tests(ens_tests)
