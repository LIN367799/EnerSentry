// tests/unit/test_alarm_query.cpp —— IAlarmAccess::queryAlarms Tier 2/3 单测（切片 36，FR-AL-11）。
// 覆盖：① 全量 DESC 排序（含 begin=0 全历史路径）② level 过滤 ③ status 过滤
//       ④ pointId 过滤 ⑤ 触发时间窗过滤 ⑥ limit 截断 ⑦ 跨月路由合并
// 写入端直用 SQLiteDataAccess::insertAlarmRecords（不经 store，聚焦查询语义）。

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <vector>

#include <QDate>
#include <QDateTime>
#include <QTemporaryDir>
#include <QTime>

#include "datahub/AlarmRecord.h"
#include "datahub/IAlarmAccess.h"
#include "datahub/SQLiteDataAccess.h"

using ens::datahub::AlarmQueryFilter;
using ens::datahub::AlarmRecord;
using ens::datahub::SQLiteDataAccess;

namespace {

class TempRootDir {
public:
    TempRootDir() {
        m_tmp.reset(new QTemporaryDir());
        if (!m_tmp->isValid()) throw std::runtime_error("QTemporaryDir failed");
    }
    QString path() const { return m_tmp->path(); }
private:
    std::unique_ptr<QTemporaryDir> m_tmp;
};

uint64_t nowMs() { return static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()); }

AlarmRecord mkRec(uint64_t id, uint32_t pointId, int level, int status, uint64_t trigger,
                  const char* desc = "t") {
    AlarmRecord r;
    r.id = id; r.pointId = pointId; r.level = level; r.status = status;
    r.triggerTime = trigger;
    r.alarmValue = 55.5; r.threshold = 50.0;
    r.description = QString::fromLatin1(desc);
    if (status >= 2) r.recoverTime = trigger + 10000;
    if (status >= 1) { r.confirmUser = QStringLiteral("op1"); r.confirmTime = trigger + 5000; }
    return r;
}

}  // namespace

TEST_CASE("alarm query: full range returns all rows sorted desc by trigger", "[master][datahub][alarm-query]") {
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    const uint64_t now = nowMs();
    std::vector<AlarmRecord> recs = {
        mkRec(1, 1, 1, 0, now - 3000),
        mkRec(2, 2, 2, 0, now - 2000),
        mkRec(3, 3, 0, 2, now - 1000),
    };
    REQUIRE(dal.insertAlarmRecords(recs));

    AlarmQueryFilter f;                       // begin=0 → 全历史
    const auto rows = dal.queryAlarms(f);
    REQUIRE(rows.size() == 3);
    REQUIRE(rows[0].triggerTime > rows[1].triggerTime);   // DESC
    REQUIRE(rows[1].triggerTime > rows[2].triggerTime);
    REQUIRE(rows[0].id == 3);
    REQUIRE(rows[0].level == 0);
    REQUIRE(rows[0].status == 2);
    REQUIRE(rows[0].confirmUser == QStringLiteral("op1"));
}

TEST_CASE("alarm query: filter by level", "[master][datahub][alarm-query]") {
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    const uint64_t now = nowMs();
    REQUIRE(dal.insertAlarmRecords({
        mkRec(1, 1, 0, 0, now - 3000),
        mkRec(2, 1, 1, 0, now - 2000),
        mkRec(3, 1, 2, 0, now - 1000),
    }));

    AlarmQueryFilter f;
    f.beginMs = now - 7 * 24 * 3600ULL * 1000;
    f.level = 2;
    const auto rows = dal.queryAlarms(f);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].id == 3);
}

TEST_CASE("alarm query: filter by status", "[master][datahub][alarm-query]") {
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    const uint64_t now = nowMs();
    REQUIRE(dal.insertAlarmRecords({
        mkRec(1, 1, 1, 0, now - 3000),
        mkRec(2, 1, 1, 1, now - 2000),
        mkRec(3, 1, 1, 2, now - 1000),
    }));

    AlarmQueryFilter f;
    f.beginMs = now - 7 * 24 * 3600ULL * 1000;
    f.status = 1;                              // Confirmed 仅一条
    const auto rows = dal.queryAlarms(f);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].id == 2);
    REQUIRE(rows[0].confirmTime > 0);
}

TEST_CASE("alarm query: filter by point id", "[master][datahub][alarm-query]") {
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    const uint64_t now = nowMs();
    REQUIRE(dal.insertAlarmRecords({
        mkRec(1, 10, 1, 0, now - 3000),
        mkRec(2, 20, 1, 0, now - 2000),
        mkRec(3, 10, 2, 0, now - 1000),
    }));

    AlarmQueryFilter f;
    f.beginMs = now - 7 * 24 * 3600ULL * 1000;
    f.pointId = 20;
    const auto rows = dal.queryAlarms(f);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].id == 2);
}

TEST_CASE("alarm query: filter by trigger time window", "[master][datahub][alarm-query]") {
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    const uint64_t now = nowMs();
    REQUIRE(dal.insertAlarmRecords({
        mkRec(1, 1, 1, 0, now - 300000),      // 5 分钟前（窗外）
        mkRec(2, 1, 1, 0, now - 60000),        // 1 分钟前（窗内）
        mkRec(3, 1, 1, 0, now - 1000),         // 1 秒前（窗内）
    }));

    AlarmQueryFilter f;
    f.beginMs = now - 120000;                  // 最近 2 分钟
    f.endMs   = now;
    const auto rows = dal.queryAlarms(f);
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0].id == 3);
    REQUIRE(rows[1].id == 2);
}

TEST_CASE("alarm query: limit caps newest rows", "[master][datahub][alarm-query]") {
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    const uint64_t now = nowMs();
    std::vector<AlarmRecord> recs;
    for (uint64_t i = 1; i <= 5; ++i) {
        recs.push_back(mkRec(i, 1, 1, 0, now - (6 - i) * 1000));   // id5 最新
    }
    REQUIRE(dal.insertAlarmRecords(recs));

    AlarmQueryFilter f;
    f.beginMs = now - 7 * 24 * 3600ULL * 1000;
    f.limit = 2;
    const auto rows = dal.queryAlarms(f);
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0].id == 5);
    REQUIRE(rows[1].id == 4);
}

TEST_CASE("alarm query: crosses month boundary and merges desc", "[master][datahub][alarm-query]") {
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    const QDate today = QDate::currentDate();
    // 上月中旬一条 + 本月 1 日一条
    const QDate prevMonth = today.addMonths(-1);
    const uint64_t prevTs = static_cast<uint64_t>(QDateTime(
        QDate(prevMonth.year(), prevMonth.month(), 15), QTime(12, 0, 0)).toMSecsSinceEpoch());
    const uint64_t curTs = static_cast<uint64_t>(QDateTime(
        QDate(today.year(), today.month(), 1), QTime(0, 0, 1)).toMSecsSinceEpoch());
    // ⚠ insertAlarmRecords 契约：单批同月（风暴窗口内同月）；跨月须分批写入
    REQUIRE(dal.insertAlarmRecords({mkRec(101, 1, 1, 0, prevTs)}));
    REQUIRE(dal.insertAlarmRecords({mkRec(102, 1, 2, 0, curTs)}));

    const uint64_t begin = static_cast<uint64_t>(QDateTime(
        QDate(prevMonth.year(), prevMonth.month(), 1), QTime(0, 0, 0)).toMSecsSinceEpoch());
    AlarmQueryFilter f;
    f.beginMs = begin;
    const auto rows = dal.queryAlarms(f);
    REQUIRE(rows.size() == 2);                 // 跨两月库合并
    REQUIRE(rows[0].id == 102);                // 本月记录更新 → 排前
    REQUIRE(rows[1].id == 101);
}
