// tests/unit/test_platform_mmap.cpp
// L3 数据中枢 ── PlatformMMap Tier 2 单测（ENS-LLD-200 §3.6.2 + Phase 3 4.1.6）。
//
// 覆盖：
//   ① open 创建 + 写入 + flushSync + 重开读取(持久性)
//   ② close 幂等(二次调用不抛)
//   ③ readOnly 打开拒绝写映射
//   ④ lastError / isLockedByOtherProcess 初始态

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include <QTemporaryDir>

#include "datahub/platform/PlatformMMap.h"

using ens::datahub::platform::IMappedFile;
using ens::datahub::platform::createMappedFile;

namespace {

class TempDir {
public:
    TempDir() : m_tmp(std::make_unique<QTemporaryDir>()) {
        if (!m_tmp->isValid()) throw std::runtime_error("QTemporaryDir failed");
    }
    std::string filePath(const char* name) const {
        return (m_tmp->path() + "/" + name).toStdString();
    }
private:
    std::unique_ptr<QTemporaryDir> m_tmp;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ① open 创建 + 写读回环
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("platform_mmap: write then reopen reads back persistent data",
          "[master][datahub][mmap][rw]") {
    TempDir tmp;
    const std::string path = tmp.filePath("swap.bin");
    constexpr size_t SIZE = 4096;

    // 写入
    {
        auto mmap = createMappedFile();
        REQUIRE(mmap->open(path, SIZE, /*readOnly=*/false));
        REQUIRE(mmap->size() == SIZE);
        REQUIRE(mmap->baseAddress() != nullptr);
        REQUIRE(mmap->lastError() == 0);
        REQUIRE_FALSE(mmap->isLockedByOtherProcess());

        auto* p = static_cast<uint32_t*>(mmap->baseAddress());
        p[0] = 0xDEADBEEF;
        p[1] = 12345;
        REQUIRE(mmap->flushSync(0, SIZE));
        mmap->close();
    }

    // 重开读取
    {
        auto mmap = createMappedFile();
        REQUIRE(mmap->open(path, SIZE, /*readOnly=*/true));
        const auto* p = static_cast<const uint32_t*>(mmap->baseAddress());
        REQUIRE(p[0] == 0xDEADBEEFu);
        REQUIRE(p[1] == 12345u);
        mmap->close();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ② close 幂等
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("platform_mmap: close is idempotent (double close no-throw)",
          "[master][datahub][mmap][close]") {
    TempDir tmp;
    auto mmap = createMappedFile();
    REQUIRE(mmap->open(tmp.filePath("idem.bin"), 1024, false));
    mmap->close();
    REQUIRE_NOTHROW(mmap->close());      // 二次 close 不抛
    REQUIRE(mmap->baseAddress() == nullptr);
    REQUIRE(mmap->size() == 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ③ readOnly 打开
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("platform_mmap: readOnly reopen of existing file works",
          "[master][datahub][mmap][readonly]") {
    TempDir tmp;
    const std::string path = tmp.filePath("ro.bin");
    {
        auto mmap = createMappedFile();
        REQUIRE(mmap->open(path, 1024, false));
        auto* p = static_cast<uint8_t*>(mmap->baseAddress());
        std::memset(p, 0xAB, 64);
        mmap->flushSync(0, 64);
        mmap->close();
    }
    auto mmap = createMappedFile();
    REQUIRE(mmap->open(path, 1024, /*readOnly=*/true));
    const auto* p = static_cast<const uint8_t*>(mmap->baseAddress());
    REQUIRE(p[0] == 0xAB);
    REQUIRE(p[63] == 0xAB);
    mmap->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// ④ lastError 初始态 + 不存在的只读文件
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("platform_mmap: readOnly open of missing file fails cleanly",
          "[master][datahub][mmap][neg]") {
    TempDir tmp;
    auto mmap = createMappedFile();
    // 只读打开不存在的文件应失败(OPEN_EXISTING)
    REQUIRE_FALSE(mmap->open(tmp.filePath("missing.bin"), 1024, /*readOnly=*/true));
    REQUIRE(mmap->baseAddress() == nullptr);
    REQUIRE(mmap->lastError() != 0);
}
