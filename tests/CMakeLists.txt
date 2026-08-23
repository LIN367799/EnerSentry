# ens_tests：Catch2 单测入口
# Step 0 测试地基（ENS-DEV-GUIDE §1.4）：在写第一行业务代码前铺好，后续每步即写即验。
add_executable(ens_tests
    unit/test_sanity.cpp
    unit/test_txid_allocator.cpp
    unit/test_accumulator.cpp)
# 链 ens::protocol 以获得 ModbusStreamAccumulator / TransactionIdAllocator 符号与头文件；
# 其 PUBLIC 依赖（channel / 3rdparty）随之透传，Catch2 经 3rdparty 注入。
target_link_libraries(ens_tests PRIVATE ens::3rdparty ens::protocol)

include(Catch)
catch_discover_tests(ens_tests)
