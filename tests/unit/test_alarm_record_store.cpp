// tests/unit/test_alarm_record_store.cpp
// L4 业务层 ── AlarmRecordStore Tier 2/3 单测（切片 35，FR-AL-13 数据面）。
// 覆盖：① 触发 → Active 记录落库（FR-AL-13 字段值正确）
//       ② 恢复 → status=2 + recover_time 回填
//       ③ 确认 → status=1 + confirm_user/confirm_time（信号带操作人）
//       ④ 先确认后恢复 → 终态 status=2 且 confirm 字段保留
//       ⑤ dataDir 空（--data-dir 缺省）→ store 禁用 no-op
//       ⑥ dal 批量接口：单事务落 2 行（风暴批复用路径）

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <vector>

#include <QDateTime>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>

#include "business/AlarmEngine.h"
#include "business/AlarmRecordStore.h"
#include "datahub/AlarmRecord.h"
#include "datahub/SQLiteDataAccess.h"

using ens::business::AlarmEngine;
using ens::business::AlarmEvent;
using ens::business::AlarmLevel;
using ens::business::AlarmRecordStore;
using ens::business::AlarmRule;
using ens::business::AlarmStormConfig;
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

/// 即时规则（on/off-delay=0 免 sleep；suppress=0 免同源抑制）
AlarmRule makeRule(uint32_t pid, float on, float off,
                   AlarmLevel lvl = AlarmLevel::Warning) {
    AlarmRule r;
    r.pointId           = pid;
    r.level             = lvl;
    r.direction         = ens::business::AlarmDirection::High;
    r.onThreshold       = on;
    r.offThreshold      = off;
    r.enabled           = true;
    r.onDelayMs         = 0;
    r.offDelayMs        = 0;
    r.suppressWindowMs  = 0;
    return r;
}

/// 回读告警库（独立只读连接；WAL 允许与写入连接并发）
struct AlarmRow {
    qlonglong id = 0, pointId = 0, level = 0, status = 0;
    qlonglong triggerTime = 0, recoverTime = 0, confirmTime = 0;
    QString   confirmUser;
    double    alarmValue = 0.0, threshold = 0.0;
    QString   description;
};
std::vector<AlarmRow> queryAlarmRows(const QString& root, uint64_t nowTs) {
    static int connIdx = 0;
    const QString connName = QStringLiteral("ars_read_%1").arg(++connIdx);
    const QString yyyymm =
        QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(nowTs)).toString(QStringLiteral("yyyyMM"));
    const QString dbPath = SQLiteDataAccess::getAlarmDatabasePath(root, nowTs);
    const QString table  = QStringLiteral("alarm_record_%1").arg(yyyymm);

    std::vector<AlarmRow> out;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        REQUIRE(db.open());
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT id, point_id, level, status, trigger_time, recover_time,"
            " confirm_user, confirm_time, alarm_value, threshold, description"
            " FROM %1 ORDER BY id").arg(table));
        REQUIRE(q.exec());
        while (q.next()) {
            AlarmRow r;
            r.id           = q.value(0).toLongLong();
            r.pointId      = q.value(1).toLongLong();
            r.level        = q.value(2).toLongLong();
            r.status       = q.value(3).toLongLong();
            r.triggerTime  = q.value(4).toLongLong();
            r.recoverTime  = q.value(5).toLongLong();
            r.confirmUser  = q.value(6).toString();
            r.confirmTime  = q.value(7).toLongLong();
            r.alarmValue   = q.value(8).toDouble();
            r.threshold    = q.value(9).toDouble();
            r.description  = q.value(10).toString();
            out.push_back(r);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
    return out;
}

uint64_t nowMs() { return static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()); }

}  // namespace

TEST_CASE("alarm store: trigger persists active record with FR-AL-13 fields", "[master][business][alarm-store]") {
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    AlarmEngine eng;
    AlarmRecordStore store(&eng, &dal);
    REQUIRE(store.isEnabled());

    uint64_t lastId = 0;
    QObject::connect(&eng, &AlarmEngine::alarmTriggered,
                     [&lastId](const AlarmEvent& ev) { lastId = ev.id; });

    eng.loadRules({makeRule(7, 50.0f, 40.0f, AlarmLevel::Critical)});
    const uint64_t t0 = nowMs();
    eng.onDataUpdated(7, t0, 66.5f);   // on-delay=0 → 立即触发

    const auto rows = queryAlarmRows(tmp.path(), nowMs());
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].pointId == 7);
    REQUIRE(rows[0].level == static_cast<qlonglong>(AlarmLevel::Critical));
    REQUIRE(rows[0].status == 0);                          // Active
    REQUIRE(rows[0].triggerTime >= static_cast<qlonglong>(t0));
    REQUIRE(rows[0].recoverTime == 0);
    REQUIRE(rows[0].confirmTime == 0);
    REQUIRE(rows[0].confirmUser.isEmpty());
    REQUIRE(rows[0].alarmValue == Catch::Approx(66.5));
    REQUIRE(rows[0].threshold == Catch::Approx(50.0));
    REQUIRE_FALSE(rows[0].description.isEmpty());
    REQUIRE(rows[0].id == static_cast<qlonglong>(lastId));
    REQUIRE(store.droppedCount() == 0);
}

TEST_CASE("alarm store: recovery updates status and recover_time", "[master][business][alarm-store]") {
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    AlarmEngine eng;
    AlarmRecordStore store(&eng, &dal);

    eng.loadRules({makeRule(3, 60.0f, 50.0f)});
    eng.onDataUpdated(3, nowMs(), 70.0f);
    REQUIRE(queryAlarmRows(tmp.path(), nowMs()).size() == 1);

    eng.onDataUpdated(3, nowMs(), 30.0f);   // 回落 off 侧 → 立即恢复
    const auto rows = queryAlarmRows(tmp.path(), nowMs());
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].status == 2);                          // Recovered
    REQUIRE(rows[0].recoverTime > 0);
    REQUIRE(store.droppedCount() == 0);
}

TEST_CASE("alarm store: acknowledge persists user and time", "[master][business][alarm-store]") {
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    AlarmEngine eng;
    AlarmRecordStore store(&eng, &dal);

    uint64_t lastId = 0;
    QObject::connect(&eng, &AlarmEngine::alarmTriggered,
                     [&lastId](const AlarmEvent& ev) { lastId = ev.id; });

    eng.loadRules({makeRule(9, 80.0f, 70.0f)});
    eng.onDataUpdated(9, nowMs(), 90.0f);
    REQUIRE(lastId > 0);

    eng.acknowledgeAlarm(lastId, QStringLiteral("operator1"));
    const auto rows = queryAlarmRows(tmp.path(), nowMs());
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].status == 1);                          // Confirmed
    REQUIRE(rows[0].confirmUser == QStringLiteral("operator1"));
    REQUIRE(rows[0].confirmTime > 0);
    REQUIRE(rows[0].recoverTime == 0);                     // 未恢复
    REQUIRE(store.droppedCount() == 0);
}

TEST_CASE("alarm store: confirm then recover keeps confirm fields", "[master][business][alarm-store]") {
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    AlarmEngine eng;
    AlarmRecordStore store(&eng, &dal);

    uint64_t lastId = 0;
    QObject::connect(&eng, &AlarmEngine::alarmTriggered,
                     [&lastId](const AlarmEvent& ev) { lastId = ev.id; });

    eng.loadRules({makeRule(5, 60.0f, 50.0f)});
    eng.onDataUpdated(5, nowMs(), 75.0f);
    eng.acknowledgeAlarm(lastId, QStringLiteral("engineer_x"));
    eng.onDataUpdated(5, nowMs(), 20.0f);   // 恢复

    const auto rows = queryAlarmRows(tmp.path(), nowMs());
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].status == 2);                          // 终态 Recovered
    REQUIRE(rows[0].recoverTime > 0);
    REQUIRE(rows[0].confirmUser == QStringLiteral("engineer_x"));   // 确认字段保留
    REQUIRE(rows[0].confirmTime > 0);
    REQUIRE(store.droppedCount() == 0);
}

TEST_CASE("alarm store: disabled when data dir empty", "[master][business][alarm-store]") {
    SQLiteDataAccess dal(QString{});    // dataDir 空（--data-dir 缺省语义；{} 防最烦解析）
    AlarmEngine eng;
    AlarmRecordStore store(&eng, &dal);
    REQUIRE_FALSE(store.isEnabled());

    eng.loadRules({makeRule(1, 60.0f, 50.0f)});
    eng.onDataUpdated(1, nowMs(), 70.0f);   // 触发但不落盘
    REQUIRE(store.droppedCount() == 0);
    // 不产生 alarm 目录/文件：检查当前工作目录未被污染（禁用时 store 不触 dal 写路径）
    REQUIRE_FALSE(QFile::exists(QStringLiteral("alarm")));
}

TEST_CASE("alarm store: dal batch insert single transaction for storm path", "[master][business][alarm-store]") {
    TempRootDir tmp;
    SQLiteDataAccess dal(tmp.path());
    const uint64_t t0 = nowMs();

    AlarmRecord a1;
    a1.id = 101; a1.pointId = 1; a1.level = 1; a1.status = 0;
    a1.triggerTime = t0; a1.alarmValue = 55.0; a1.threshold = 50.0;
    a1.description = QStringLiteral("point=1 value=55");
    AlarmRecord a2 = a1;
    a2.id = 102; a2.pointId = 2; a2.description = QStringLiteral("point=2 value=55");

    REQUIRE(dal.insertAlarmRecords({a1, a2}));
    const auto rows = queryAlarmRows(tmp.path(), t0);
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0].id == 101);
    REQUIRE(rows[1].id == 102);
    REQUIRE(rows[0].pointId == 1);
    REQUIRE(rows[1].pointId == 2);
}
