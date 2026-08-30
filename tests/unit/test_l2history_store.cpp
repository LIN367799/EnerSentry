// tests/unit/test_l2history_store.cpp
// L3 数据中枢 ── L2HistoryStore Tier 2/3 单测（ENS-LLD-200 §4.5 + Phase 3 4.1.5）。
//
// 覆盖：
//   ① enqueueSample + flush 双缓冲 swap:pendingCount 归零
//   ② 落库验证:flush 后可查
//   ③ 背压:超 capacity 丢最老 + droppedCount
//   ④ 多条分桶:同月多测点 → 同一月库多行
//   ⑤ null dal 防御
//   ⑥ 析构时自动 flush
//   ⑦ Tier 3 5000 pts/s 跑批(单测粒度:5K 样本月库路由 + 事务无半成品)

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>

#include "common/test_helpers.h"
#include "datahub/DownSampler.h"
#include "datahub/L2HistoryStore.h"
#include "datahub/SQLiteDataAccess.h"

using ens::datahub::DownSampledSample;
using ens::datahub::HistoryGranularity;
using ens::datahub::L2HistoryStore;
using ens::datahub::SQLiteDataAccess;
using ens::test::appInstance;

namespace {

class TempRootDir {
public:
    TempRootDir() : m_tmp(std::make_unique<QTemporaryDir>()) {
        if (!m_tmp->isValid()) throw std::runtime_error("QTemporaryDir failed");
    }
    QString path() const { return m_tmp->path(); }
private:
    std::unique_ptr<QTemporaryDir> m_tmp;
};

DownSampledSample mkSample(uint32_t pid, uint64_t ts, float mn, float mx) {
    DownSampledSample s;
    s.pointId = pid;
    s.timestamp = ts;
    s.minValue = mn;
    s.maxValue = mx;
    s.avgValue = (mn + mx) / 2.0f;
    s.sampleCount = 1;
    return s;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ① enqueue + flush
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("l2history_store: enqueue then flush drains buffer to DB",
          "[master][datahub][l2store][drain]") {
    appInstance();
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    L2HistoryStore store(&dal);

    const uint64_t ts = 1755268800000ULL;
    for (uint32_t i = 1; i <= 5; ++i) {
        store.enqueueSample(mkSample(i, ts, 1.0f * i, 2.0f * i));
    }
    REQUIRE(store.pendingCount() == 5u);
    REQUIRE(store.flush());
    REQUIRE(store.pendingCount() == 0u);
    REQUIRE(store.droppedCount() == 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ② 落库验证
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("l2history_store: flushed samples are queryable in month DB",
          "[master][datahub][l2store][query]") {
    appInstance();
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    L2HistoryStore store(&dal);
    const uint64_t ts = 1755268800000ULL;

    for (uint32_t i = 1; i <= 10; ++i) {
        store.enqueueSample(mkSample(i, ts, 10.0f, 20.0f));
    }
    REQUIRE(store.flush());

    // 验证月库 10 行
    const QString dbPath = dal.getDatabasePath(ts);
    QSqlDatabase check = QSqlDatabase::addDatabase("QSQLITE", "l2_check");
    check.setDatabaseName(dbPath);
    REQUIRE(check.open());
    QSqlQuery q(check);
    REQUIRE(q.exec("SELECT COUNT(*) FROM history_1s_202508"));
    REQUIRE(q.next());
    REQUIRE(q.value(0).toInt() == 10);
    check.close();
    QSqlDatabase::removeDatabase("l2_check");
}

// ─────────────────────────────────────────────────────────────────────────────
// ③ 背压
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("l2history_store: backpressure drops oldest, increments droppedCount",
          "[master][datahub][l2store][backpressure]") {
    appInstance();
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    L2HistoryStore store(&dal);
    store.setCapacity(10);                                // 容量 10,超过触发背压

    const uint64_t ts = 1755268800000ULL;
    for (uint32_t i = 1; i <= 20; ++i) {
        store.enqueueSample(mkSample(i, ts, 1.0f, 2.0f));
    }
    REQUIRE(store.pendingCount() == 10u);                // 截到 capacity
    REQUIRE(store.droppedCount() == 10u);                // 丢 10 个最老
    REQUIRE(store.flush());
    // 落库的 10 个是 i=11..20(最新 10)
    const QString dbPath = dal.getDatabasePath(ts);
    QSqlDatabase check = QSqlDatabase::addDatabase("QSQLITE", "l2_back");
    check.setDatabaseName(dbPath);
    REQUIRE(check.open());
    QSqlQuery q(check);
    q.prepare("SELECT MIN(point_id), MAX(point_id) FROM history_1s_202508");
    REQUIRE(q.exec());
    REQUIRE(q.next());
    REQUIRE(q.value(0).toInt() == 11);                    // 最早是 pid=11
    REQUIRE(q.value(1).toInt() == 20);                    // 最新是 pid=20
    check.close();
    QSqlDatabase::removeDatabase("l2_back");
}

// ─────────────────────────────────────────────────────────────────────────────
// ④ null dal 防御
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("l2history_store: flush with null dal returns false",
          "[master][datahub][l2store][neg]") {
    appInstance();
    L2HistoryStore store(nullptr);
    store.enqueueSample(mkSample(1, 1755268800000ULL, 1.0f, 2.0f));
    REQUIRE_FALSE(store.flush());
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑤ 析构时自动 flush
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("l2history_store: destructor flushes pending samples",
          "[master][datahub][l2store][dtor]") {
    appInstance();
    TempRootDir tmp;
    const uint64_t ts = 1755268800000ULL;
    const QString dbPath = [&] {
        SQLiteDataAccess dal(tmp.path());
        L2HistoryStore store(&dal);
        for (uint32_t i = 1; i <= 3; ++i) {
            store.enqueueSample(mkSample(i, ts, 1.0f, 2.0f));
        }
        REQUIRE(store.pendingCount() == 3u);
        // store 析构 → 自动 flush
        return dal.getDatabasePath(ts);
    }();

    QSqlDatabase check = QSqlDatabase::addDatabase("QSQLITE", "l2_dtor");
    check.setDatabaseName(dbPath);
    REQUIRE(check.open());
    QSqlQuery q(check);
    REQUIRE(q.exec("SELECT COUNT(*) FROM history_1s_202508"));
    REQUIRE(q.next());
    REQUIRE(q.value(0).toInt() == 3);
    check.close();
    QSqlDatabase::removeDatabase("l2_dtor");
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑥ Tier 3:5000 pts/s 跑批(单测粒度)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("l2history_store: 5000 samples batched into month DB without loss",
          "[master][datahub][l2store][stress][tier3]") {
    appInstance();
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    L2HistoryStore store(&dal);
    const uint64_t ts = 1755268800000ULL;
    constexpr int N = 5000;

    for (int i = 1; i <= N; ++i) {
        store.enqueueSample(mkSample(static_cast<uint32_t>(i), ts, 1.0f, 2.0f));
    }
    REQUIRE(store.pendingCount() == static_cast<size_t>(N));
    REQUIRE(store.flush());
    REQUIRE(store.pendingCount() == 0u);

    // 验证 5000 行全部落库 + TransactionGuard 无半成品
    const QString dbPath = dal.getDatabasePath(ts);
    QSqlDatabase check = QSqlDatabase::addDatabase("QSQLITE", "l2_stress");
    check.setDatabaseName(dbPath);
    REQUIRE(check.open());
    QSqlQuery q(check);
    REQUIRE(q.exec("SELECT COUNT(*), MIN(point_id), MAX(point_id) FROM history_1s_202508"));
    REQUIRE(q.next());
    REQUIRE(q.value(0).toInt() == N);
    REQUIRE(q.value(1).toInt() == 1);
    REQUIRE(q.value(2).toInt() == N);
    check.close();
    QSqlDatabase::removeDatabase("l2_stress");
}
