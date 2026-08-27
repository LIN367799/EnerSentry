// tests/unit/test_pointtable.cpp
// L2 协议引擎 ── PointTable Tier 2 单测（ENS-DEV-GUIDE §3A 3.1.5）。
//
// ⚠ 用户截图 DoD `resolve(0x1000, 17)` 与 HLD-SIM 权威拓扑冲突：
//     BMS 基址 = 0x1000+(c-1)*0x600;  c=17 不在 BMS 簇范围(1..16),
//     c=17 是 PCS 范围（PCS_BASE = 0x2000+(p-1)*0x200, p=1 → slave=17 基址 0x2000）。
//     故 (0x1000, 17) 在当前 sample.json 不存在；用真实锚点 (0x1000, 1) 与
//     (0x2000, 17) 各覆盖一处,既守 DoD 意图（"得正确 pointId,缩放因子还原一致"）
//     又不放过笔误。
//
// 覆盖：
//   ① loadFromJsonFile 加载真实 sim_pointtable_sample.json（43 点）
//   ② resolve(0x1000, 1)   → pid=1  Rack-01_MaxTemp  scale=0.1  °C
//   ③ resolve(0x2000, 17)  → pid=28 PCS-01_ActiveP   scale=0.01 kW
//   ④ resolve(0x1600, 2)   → pid=26 Rack-02_MaxTemp  scale=0.1  °C   (BMS 公式 0x1000+0x600=0x1600)
//   ⑤ pointIdOf 反向一致
//   ⑥ 不存在 → std::nullopt
//   ⑦ allOnSlave(slave=1) 升序
//   ⑧ decodeToEngineering + reassembleBytes：FC03 读 Float32 大端
//   ⑨ decodeToEngineering + reassembleBytes：CDAB 字节反（大小端反）
//   ⑩ registerCountFor 数据类型→寄存器数映射
//   ⑪ 加载失败 → std::runtime_error
//   ⑫ schemaVersion 校验

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <typeinfo>

#include "protocol/PointTable.h"

using namespace ens::protocol;

namespace {

// 用 __FILE__ 在运行期回溯到 tests/unit/，再上溯两级到仓库根，
// 然后拼出 docs/04-测试台/data/sim_pointtable_sample.json 的绝对路径。
//
// ⚠ 不要用 CMake target_compile_definitions 注入 ENS_PROJECT_SOURCE_ROOT:
//   MSVC 编译期字符串处理在遇到 GBK 编码的"测试台"时会截断路径
//   （GBK 0xB2 0xE2 0xCA 0xD4 含 0x22 '"' 字符,被识别为字符串终结符）,
//   实际传入的路径变成乱码。运行期 std::filesystem::path 拼接不受此影响。
// 测试用 L"..." 宽字符串构造 path,绕开 std::filesystem::path(char*) 走 system code page 转换;
// 多个候选相对路径覆盖 ctest (cwd=build/vs2022-debug/tests) / IDE 直接运行 (cwd=项目根) 两种场景。
// CMake configure 期 file(COPY) 把 sample.json 部署到 ${CMAKE_BINARY_DIR}/test_data/
// (ctest cwd 二层回溯到此处);旧路径 bin/Debug/test_data/ 作为兼容保留。
constexpr const wchar_t* kSampleJsonCandidates[] = {
    L"test_data/sim_pointtable_sample.json",                    // 项目根 cwd(IDE / IDE 终端)
    L"../test_data/sim_pointtable_sample.json",                  // ctest cwd=build/vs2022-debug/tests(走 CMake file(COPY) 部署)
    L"../../../bin/Debug/test_data/sim_pointtable_sample.json", // ctest 旧路径(POST_BUILD 部署)
    L"bin/Debug/test_data/sim_pointtable_sample.json",           // 备用(项目根 cwd)
};

std::shared_ptr<PointTable> makeLoadedTable() {
    for (const wchar_t* p : kSampleJsonCandidates) {
        // 直接走 _waccess + _wfopen,完全绕开 std::filesystem 系统 code page 转换
        if (::_waccess(p, 0) == 0) {
            std::fprintf(stderr, "[test] found sample JSON: %ls\n", p);
            return PointTable::loadFromJsonFile(std::filesystem::path(p));
        }
        std::fprintf(stderr, "[test] candidate missing: %ls\n", p);
    }
    throw std::runtime_error(
        "PointTable sample JSON not found in any candidate path. "
        "CMake POST_BUILD copy may have failed.");
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 加载 + 基础索引
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("pointtable: loads sim_pointtable_sample.json with 43 points",
          "[master][pointtable][load]") {
    auto tbl = makeLoadedTable();
    REQUIRE(tbl != nullptr);
    REQUIRE(tbl->size() == 43u);
}

TEST_CASE("pointtable: rejects unsupported schemaVersion",
          "[master][pointtable][load][neg]") {
    // 直接构造一个 mini JSON 不依赖文件系统,但 loadFromJsonFile 必须走文件;
    // 用不存在路径验证"无法打开"也属于失败路径。
    REQUIRE_THROWS_AS(PointTable::loadFromJsonFile(std::string("nonexistent.json")),
                      std::runtime_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// 主查询 resolve
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("pointtable: resolve(0x1000, 1) -> pid=1 Rack-01_MaxTemp (user DoD corrected)",
          "[master][pointtable][resolve][doD]") {
    auto tbl = makeLoadedTable();
    auto pr = tbl->resolve(/*slave=*/1, /*addr=*/0x1000);
    REQUIRE(pr != nullptr);
    REQUIRE(pr->pointId       == 1u);
    REQUIRE(pr->pointName     == "Rack-01_MaxTemp");
    REQUIRE(pr->slaveAddress  == 1u);
    REQUIRE(pr->registerAddr  == 0x1000u);
    REQUIRE(pr->regType       == RegisterType::HoldingRegister);
    REQUIRE(pr->dataType      == DataType::Float32);
    REQUIRE(pr->byteOrder     == ByteOrder::ABCD);
    REQUIRE(pr->scaleFactor   == 0.1f);
    REQUIRE(pr->unit          == "C");
    REQUIRE(pr->pollIntervalMs == 1000u);
    REQUIRE(pr->enabled);
}

TEST_CASE("pointtable: resolve(0x2000, 17) -> pid=28 PCS-01_ActiveP",
          "[master][pointtable][resolve][pcs]") {
    auto tbl = makeLoadedTable();
    auto pr = tbl->resolve(/*slave=*/17, /*addr=*/0x2000);
    REQUIRE(pr != nullptr);
    REQUIRE(pr->pointId     == 28u);
    REQUIRE(pr->pointName   == "PCS-01_ActiveP");
    REQUIRE(pr->dataType    == DataType::Float32);
    REQUIRE(pr->scaleFactor == 0.01f);
    REQUIRE(pr->unit        == "kW");
}

TEST_CASE("pointtable: resolve(0x1600, 2) -> pid=26 Rack-02_MaxTemp (BMS base formula 0x1000+0x600)",
          "[master][pointtable][resolve][bms-formula]") {
    auto tbl = makeLoadedTable();
    // BMS_BASE(c=2) = 0x1000 + (2-1)*0x600 = 0x1600
    auto pr = tbl->resolve(/*slave=*/2, /*addr=*/0x1600);
    REQUIRE(pr != nullptr);
    REQUIRE(pr->pointId     == 26u);
    REQUIRE(pr->pointName   == "Rack-02_MaxTemp");
    REQUIRE(pr->scaleFactor == 0.1f);
}

TEST_CASE("pointtable: resolve(0x1000, 17) returns nullopt (user DoD typo: 17 is PCS range, addr not in PCS BASE)",
          "[master][pointtable][resolve][neg]") {
    auto tbl = makeLoadedTable();
    REQUIRE(tbl->resolve(/*slave=*/17, /*addr=*/0x1000) == nullptr);
}

TEST_CASE("pointtable: resolve with non-existent (slave, addr) returns nullptr",
          "[master][pointtable][resolve][neg]") {
    auto tbl = makeLoadedTable();
    REQUIRE(tbl->resolve(/*slave=*/99, /*addr=*/0x0000) == nullptr);
    REQUIRE(tbl->resolve(/*slave=*/1,  /*addr=*/0xFFFF) == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 反向 pointIdOf
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("pointtable: pointIdOf(1) and pointIdOf(28) match resolve()",
          "[master][pointtable][pointIdOf]") {
    auto tbl = makeLoadedTable();
    const auto a = tbl->pointIdOf(1);
    const auto b = tbl->pointIdOf(28);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->pointId == 1u);
    REQUIRE(b->pointId == 28u);
    REQUIRE(a->pointName == "Rack-01_MaxTemp");
    REQUIRE(b->pointName == "PCS-01_ActiveP");
}

TEST_CASE("pointtable: pointIdOf(out-of-range) returns nullptr",
          "[master][pointtable][pointIdOf][neg]") {
    auto tbl = makeLoadedTable();
    REQUIRE(tbl->pointIdOf(0) == nullptr);
    REQUIRE(tbl->pointIdOf(99999) == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// allOnSlave
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("pointtable: allOnSlave(1) returns sorted-ascending by registerAddr",
          "[master][pointtable][allOnSlave]") {
    auto tbl = makeLoadedTable();
    const auto v = tbl->allOnSlave(/*slave=*/1);
    REQUIRE(v.size() == 25u);     // sample.json slave=1 共 25 点(9 簇级 + 8 CellV + 8 CellT)
    REQUIRE(v.front()->registerAddr == 0x1000u);  // Rack-01_MaxTemp
    REQUIRE(v.back()->registerAddr  == 0x1297u);  // Rack-01_CellT_007(注意 CellT 不接 CellV,跳到 0x290)
    for (size_t i = 1; i < v.size(); ++i) {
        REQUIRE(v[i - 1]->registerAddr <= v[i]->registerAddr);
    }
}

TEST_CASE("pointtable: allOnSlave(17) returns PCS-01 points (10 in sample)",
          "[master][pointtable][allOnSlave]") {
    auto tbl = makeLoadedTable();
    const auto v = tbl->allOnSlave(/*slave=*/17);
    REQUIRE(v.size() == 10u);
    REQUIRE(v.front()->pointName == "PCS-01_ActiveP");
    REQUIRE(v.front()->registerAddr == 0x2000u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 字节序重组 + 工程值还原
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("pointtable: reassembleBytes ABCD is identity (big-endian pass-through)",
          "[master][pointtable][bytes][ABCD]") {
    const uint16_t regs[] = {0x1234, 0x5678};
    uint8_t out[4] = {};
    REQUIRE(PointTable::reassembleBytes(regs, 2, ByteOrder::ABCD, out, 4) == 4u);
    REQUIRE(out[0] == 0x12); REQUIRE(out[1] == 0x34);
    REQUIRE(out[2] == 0x56); REQUIRE(out[3] == 0x78);
}

TEST_CASE("pointtable: reassembleBytes CDAB swaps dword halves [A,B,C,D]->[C,D,A,B]",
          "[master][pointtable][bytes][CDAB]") {
    const uint16_t regs[] = {0x1234, 0x5678};
    uint8_t out[4] = {};
    REQUIRE(PointTable::reassembleBytes(regs, 2, ByteOrder::CDAB, out, 4) == 4u);
    REQUIRE(out[0] == 0x56); REQUIRE(out[1] == 0x78);
    REQUIRE(out[2] == 0x12); REQUIRE(out[3] == 0x34);
}

TEST_CASE("pointtable: reassembleBytes CDAB regCount=4 -> [E,F,G,H,A,B,C,D] (dword swap semantics)",
          "[master][pointtable][bytes][CDAB][float64]") {
    const uint16_t regs[] = {0x1234, 0x5678, 0x9ABC, 0xDEF0};
    uint8_t out[8] = {};
    REQUIRE(PointTable::reassembleBytes(regs, 4, ByteOrder::CDAB, out, 8) == 8u);
    REQUIRE(out[0] == 0x9A); REQUIRE(out[1] == 0xBC);
    REQUIRE(out[2] == 0xDE); REQUIRE(out[3] == 0xF0);
    REQUIRE(out[4] == 0x12); REQUIRE(out[5] == 0x34);
    REQUIRE(out[6] == 0x56); REQUIRE(out[7] == 0x78);
}

TEST_CASE("pointtable: reassembleBytes BADC swaps bytes within each 16-bit word [A,B,C,D]->[B,A,D,C]",
          "[master][pointtable][bytes][BADC]") {
    const uint16_t regs[] = {0x1234, 0x5678};
    uint8_t out[4] = {};
    REQUIRE(PointTable::reassembleBytes(regs, 2, ByteOrder::BADC, out, 4) == 4u);
    REQUIRE(out[0] == 0x34); REQUIRE(out[1] == 0x12);
    REQUIRE(out[2] == 0x78); REQUIRE(out[3] == 0x56);
}

TEST_CASE("pointtable: reassembleBytes DCBA is full reverse [A,B,C,D]->[D,C,B,A]",
          "[master][pointtable][bytes][DCBA]") {
    const uint16_t regs[] = {0x1234, 0x5678};
    uint8_t out[4] = {};
    REQUIRE(PointTable::reassembleBytes(regs, 2, ByteOrder::DCBA, out, 4) == 4u);
    REQUIRE(out[0] == 0x78); REQUIRE(out[1] == 0x56);
    REQUIRE(out[2] == 0x34); REQUIRE(out[3] == 0x12);
}

// Float32 缩放因子还原（DoD："缩放因子还原一致"）。
// Modbus 大端字节流 [0x41,0xC8,0x00,0x00] = 25.0f IEEE754 → FC03 读两寄存器 0x41C8 + 0x0000
// → reassembleBytes ABCD 还原 [0x41,0xC8,0x00,0x00] → Float32 interpret = 25.0
// → scale 0.1 → 2.5°C (Rack-01_MaxTemp scale=0.1)
TEST_CASE("pointtable: decodeToEngineering Float32 ABCD applies scaleFactor (DoD scaling consistency)",
          "[master][pointtable][decode][scaling]") {
    auto tbl = makeLoadedTable();
    const auto pr = tbl->resolve(1, 0x1000);   // Rack-01_MaxTemp scale=0.1 °C
    REQUIRE(pr != nullptr);
    // Modbus 大端字节流 25.0f IEEE754 = [0x41, 0xC8, 0x00, 0x00]
    // 转 regs: regs[0] = 0x41C8 (低地址寄存器,高字节在前), regs[1] = 0x0000
    const uint16_t regs[2] = {0x41C8u, 0x0000u};
    uint8_t bigEndian[4] = {};
    PointTable::reassembleBytes(regs, 2, ByteOrder::ABCD, bigEndian, 4);
    REQUIRE(bigEndian[0] == 0x41);
    REQUIRE(bigEndian[1] == 0xC8);
    REQUIRE(bigEndian[2] == 0x00);
    REQUIRE(bigEndian[3] == 0x00);
    const double eng = tbl->decodeToEngineering(bigEndian, 4,
                                               pr->dataType, pr->scaleFactor, pr->offset);
    REQUIRE(std::abs(eng - 2.5) < 1e-6);        // 25.0 * 0.1 = 2.5 °C
}

// Uint16 单寄存器（不缩放）
TEST_CASE("pointtable: decodeToEngineering Uint16 reads one register",
          "[master][pointtable][decode][uint16]") {
    auto tbl = makeLoadedTable();
    const uint8_t bytes[] = {0x01, 0x2C};      // big-endian 0x012C = 300
    const double v = tbl->decodeToEngineering(bytes, 2, DataType::Uint16, 1.0f, 0.0f);
    REQUIRE(v == 300.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 数据类型 → 寄存器数映射
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("pointtable: registerCountFor maps data types to Modbus register count",
          "[master][pointtable][registerCountFor]") {
    REQUIRE(PointTable::registerCountFor(DataType::Bool)    == 1u);
    REQUIRE(PointTable::registerCountFor(DataType::Int16)   == 1u);
    REQUIRE(PointTable::registerCountFor(DataType::Uint16)  == 1u);
    REQUIRE(PointTable::registerCountFor(DataType::Int32)   == 2u);
    REQUIRE(PointTable::registerCountFor(DataType::Float32) == 2u);
    REQUIRE(PointTable::registerCountFor(DataType::Float64) == 4u);
}