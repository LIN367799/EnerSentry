// tests/unit/test_history_query.cpp —— SQLiteDataAccess::queryRange 历史查询单测（切片 24）。
// 覆盖：单月写入→查询往返 / 范围过滤 / 空区间 / 无数据点。
// ⚠ TEST_CASE 第一参数严格 ASCII（项目测试铁律）。
#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>

#include "SQLiteDataAccess.h"

using ens::datahub::SQLiteDataAccess;
using ens::datahub::DownSampledSample;
using ens::datahub::HistoryGranularity;

namespace {
// 构造 1s 粒度样本（ts 对齐秒窗口）
DownSampledSample mk(uint32_t pid, uint64_t ts, float v) {
    DownSampledSample s;
    s.pointId = pid;
    s.timestamp = ts;
    s.maxValue = v;
    s.minValue = v - 1.0f;
    s.avgValue = v - 0.5f;
    s.sampleCount = 2;
    return s;
}
}  // namespace

TEST_CASE("history_query: round-trip single month") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    SQLiteDataAccess dal(dir.path());

    // 2026-09-01 12:00:00.000 UTC（整秒）
    const uint64_t base = 1780398000000ULL;   // 2026-09-01T12:00:00Z
    std::vector<DownSampledSample> samples = {
        mk(1, base, 10.0f), mk(1, base + 1000, 11.0f), mk(1, base + 2000, 12.0f)};
    const QString dbPath = dal.getDatabasePath(base);
    REQUIRE(dal.batchInsert(dbPath, HistoryGranularity::Gran1s, samples));

    const auto out = dal.queryRange(1, base - 5000, base + 5000, HistoryGranularity::Gran1s);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0].timestamp == base);
    REQUIRE(out[1].timestamp == base + 1000);
    REQUIRE(out[2].timestamp == base + 2000);
    REQUIRE(out[2].maxValue == 12.0f);
    REQUIRE(out[2].avgValue == 11.5f);
}

TEST_CASE("history_query: range filter clips to [begin, end)") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    SQLiteDataAccess dal(dir.path());

    const uint64_t base = 1780398000000ULL;
    std::vector<DownSampledSample> samples = {
        mk(1, base, 1.0f), mk(1, base + 1000, 2.0f), mk(1, base + 2000, 3.0f)};
    REQUIRE(dal.batchInsert(dal.getDatabasePath(base), HistoryGranularity::Gran1s, samples));

    // [base, base+2000) → 仅前两条
    const auto out = dal.queryRange(1, base, base + 2000, HistoryGranularity::Gran1s);
    REQUIRE(out.size() == 2);
    REQUIRE(out.front().timestamp == base);
    REQUIRE(out.back().timestamp == base + 1000);
}

TEST_CASE("history_query: empty interval returns empty") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    SQLiteDataAccess dal(dir.path());
    const auto out = dal.queryRange(1, 2000, 1000, HistoryGranularity::Gran1s);
    REQUIRE(out.empty());
}

TEST_CASE("history_query: unknown point yields empty") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    SQLiteDataAccess dal(dir.path());

    const uint64_t base = 1780398000000ULL;
    std::vector<DownSampledSample> samples = {mk(1, base, 10.0f)};
    REQUIRE(dal.batchInsert(dal.getDatabasePath(base), HistoryGranularity::Gran1s, samples));

    const auto out = dal.queryRange(999, base - 1000, base + 1000, HistoryGranularity::Gran1s);
    REQUIRE(out.empty());
}
