# ens_tests：Catch2 单测入口（BUILD-0 仅挂 sanity 用例，后续按 ENS-DEV-GUIDE 补全）
add_executable(ens_tests
    unit/test_sanity.cpp)
target_link_libraries(ens_tests PRIVATE ens::3rdparty)

include(Catch)
catch_discover_tests(ens_tests)
