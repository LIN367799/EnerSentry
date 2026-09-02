// src/datahub/SQLiteDataAccess.cpp
// L3 数据中枢 ── SQLite 数据访问层实现（ENS-LLD-200 §4）。

#include "SQLiteDataAccess.h"

#include <atomic>
#include <algorithm>
#include <cstdint>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace ens::datahub {

namespace {

constexpr uint64_t kMaxSaneTs = 4102444800000ULL;     // 2100-01-01 UTC ms,防溢出

/// LLD-200 §4.2 PRAGMA 调优
const QStringList kPragmas = {
    "PRAGMA journal_mode = WAL",
    "PRAGMA synchronous   = NORMAL",
    "PRAGMA cache_size    = -64000",
    "PRAGMA temp_store    = MEMORY",
    "PRAGMA mmap_size     = 268435456",
    "PRAGMA busy_timeout  = 3000",
};

/// 从 dbPath(file: .../data_YYYYMM.db)反推月初时间戳(供 ensureSchema 内部建表)
uint64_t monthStartFromDbPath(const QString& dbPath) {
    const int slash = dbPath.lastIndexOf('/');
    const QString filename = (slash >= 0) ? dbPath.mid(slash + 1) : dbPath;
    if (!filename.startsWith(QStringLiteral("data_"))) return 0;
    const QString yyyymm = filename.mid(5, 6);          // 跳过 "data_"
    const QDateTime dt = QDateTime::fromString(yyyymm, QStringLiteral("yyyyMM"));
    if (!dt.isValid()) return 0;
    return static_cast<uint64_t>(dt.toMSecsSinceEpoch());
}

/// 从 dbPath(file: .../alarm_YYYYMM.db)反推月初时间戳(供 ensureAlarmSchema 内部建表)
uint64_t alarmMonthFromDbPath(const QString& dbPath) {
    const int slash = dbPath.lastIndexOf('/');
    const QString filename = (slash >= 0) ? dbPath.mid(slash + 1) : dbPath;
    if (!filename.startsWith(QStringLiteral("alarm_"))) return 0;
    const QString yyyymm = filename.mid(6, 6);          // 跳过 "alarm_"
    const QDateTime dt = QDateTime::fromString(yyyymm, QStringLiteral("yyyyMM"));
    if (!dt.isValid()) return 0;
    return static_cast<uint64_t>(dt.toMSecsSinceEpoch());
}

/// 时间戳 → 告警表名(alarm_record_YYYYMM)
QString alarmTableNameOf(uint64_t timestamp) {
    if (timestamp == 0 || timestamp > kMaxSaneTs) return {};
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(timestamp));
    if (!dt.isValid()) return {};
    return QStringLiteral("alarm_record_%1").arg(dt.toString(QStringLiteral("yyyyMM")));
}

}  // namespace

SQLiteDataAccess::SQLiteDataAccess(const QString& dataRootDir, QObject* parent)
    : QObject(parent), m_dataRootDir(dataRootDir) {}

SQLiteDataAccess::~SQLiteDataAccess() {
    closeAll();
}

QString SQLiteDataAccess::granularitySuffix(HistoryGranularity gran) {
    switch (gran) {
        case HistoryGranularity::Gran100ms: return QStringLiteral("_100ms");
        case HistoryGranularity::Gran1s:    return QStringLiteral("_1s");
        case HistoryGranularity::Gran5s:    return QStringLiteral("_5s");
        case HistoryGranularity::Gran1m:    return QStringLiteral("_1m");
    }
    return QStringLiteral("_1s");                       // 兜底
}

QString SQLiteDataAccess::getTableName(uint64_t timestamp, HistoryGranularity gran) const {
    if (timestamp == 0 || timestamp > kMaxSaneTs) {    // 边界:非法/溢出时间戳
        return {};
    }
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(timestamp));
    if (!dt.isValid()) return {};
    return QStringLiteral("history%1_%2")
        .arg(granularitySuffix(gran), dt.toString(QStringLiteral("yyyyMM")));
}

QString SQLiteDataAccess::getDatabasePath(uint64_t timestamp) const {
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(timestamp));
    const QString yyyymm = dt.toString(QStringLiteral("yyyyMM"));
    const QString monthDir = m_dataRootDir + QStringLiteral("/history/") + yyyymm;
    QDir().mkpath(monthDir);                           // 首次访问时惰性创建
    return monthDir + QStringLiteral("/data_") + yyyymm + QStringLiteral(".db");
}

QString SQLiteDataAccess::getAlarmDatabasePath(const QString& root, uint64_t timestamp) {
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(timestamp));
    const QString yyyymm = dt.toString(QStringLiteral("yyyyMM"));
    const QString monthDir = root + QStringLiteral("/alarm/") + yyyymm;
    QDir().mkpath(monthDir);
    return monthDir + QStringLiteral("/alarm_") + yyyymm + QStringLiteral(".db");
}

QString SQLiteDataAccess::makeConnName() {
    return QStringLiteral("ens_dal_%1").arg(++m_nextConnIdx);
}

bool SQLiteDataAccess::applyPragmas(QSqlDatabase& db) {
    for (const QString& p : kPragmas) {
        // Qt 5.15 QSqlDatabase::exec 返 QSqlQuery(非 bool);用 lastError 判定
        db.exec(p);
        if (db.lastError().isValid()) {
            qWarning("SQLiteDataAccess: PRAGMA failed: %s | err=%s",
                     qUtf8Printable(p), qUtf8Printable(db.lastError().text()));
            return false;
        }
    }
    return true;
}

bool SQLiteDataAccess::openMonth(uint64_t timestamp) {
    const QString dbPath = getDatabasePath(timestamp);
    if (m_connForPath.contains(dbPath)) return true;     // 已打开

    const QString connName = makeConnName();
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
    db.setDatabaseName(dbPath);
    if (!db.open()) {
        qWarning("SQLiteDataAccess: open failed: %s | err=%s",
                 qUtf8Printable(dbPath), qUtf8Printable(db.lastError().text()));
        db = QSqlDatabase();   // 销毁局部句柄，再 remove（防 "still in use" 告警）
        QSqlDatabase::removeDatabase(connName);
        return false;
    }
    if (!applyPragmas(db)) {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
        return false;
    }
    // 关键:先 insert m_connForPath 再 ensureSchema
    //   ensureSchema 内部若 m_connForPath 不含 dbPath 会再调 openMonth(ts) 触发无限递归 → 栈溢出
    m_connForPath.insert(dbPath, db);
    // 首次打开时建 1s 粒度表(其他粒度按需在 batchInsert 前 ensure)
    if (!ensureSchema(dbPath, HistoryGranularity::Gran1s)) {
        m_connForPath.remove(dbPath);   // 销毁容器拷贝
        db = QSqlDatabase();            // 销毁局部句柄
        QSqlDatabase::removeDatabase(connName);
        return false;
    }
    return true;
}

void SQLiteDataAccess::closeAll() {
    // Qt 要求 removeDatabase 时该连接无任何 QSqlDatabase 实例存活（含隐式共享拷贝），
    // 否则告警 "connection is still in use" → 先收集 connName 并销毁容器内拷贝，再 remove。
    QStringList names;
    names.reserve(m_connForPath.size());
    for (auto it = m_connForPath.begin(); it != m_connForPath.end(); ++it) {
        if (it.value().isOpen()) it.value().close();
        names.push_back(it.value().connectionName());
    }
    m_connForPath.clear();   // 销毁全部 QSqlDatabase 实例（含隐式共享句柄）
    for (const QString& n : names) {
        QSqlDatabase::removeDatabase(n);
    }
}

bool SQLiteDataAccess::ensureSchema(const QString& dbPath, HistoryGranularity gran) {
    if (!m_connForPath.contains(dbPath)) {
        // 从 dbPath 反推月初 ts(同 batchInsert,避免 mtime=0 误入 1970)
        const uint64_t ts = monthStartFromDbPath(dbPath);
        if (ts == 0) {
            qWarning("SQLiteDataAccess: ensureSchema cannot parse YYYYMM from dbPath: %s",
                     qUtf8Printable(dbPath));
            return false;
        }
        if (!openMonth(ts)) return false;
    }
    QSqlDatabase& db = m_connForPath[dbPath];
    const uint64_t ts = monthStartFromDbPath(dbPath);
    if (ts == 0) {                                       // 双保险(理论上 openMonth 已处理)
        qWarning("SQLiteDataAccess: cannot parse YYYYMM from dbPath: %s",
                 qUtf8Printable(dbPath));
        return false;
    }
    const QString table = getTableName(ts, gran);
    const QString ddl = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS %1 ("
        "  point_id     INTEGER NOT NULL,"
        "  ts           INTEGER NOT NULL,"
        "  v_max        REAL    NOT NULL,"
        "  v_min        REAL    NOT NULL,"
        "  v_avg        REAL    NOT NULL,"
        "  sample_count INTEGER NOT NULL,"
        "  PRIMARY KEY (point_id, ts)"
        ") WITHOUT ROWID").arg(table);
    // Qt 5.15 QSqlQuery::exec 返 QSqlQuery&(非 bool);用 lastError 判定
    db.exec(ddl);
    if (db.lastError().isValid()) {
        qWarning("SQLiteDataAccess: CREATE TABLE failed: %s | err=%s",
                 qUtf8Printable(ddl), qUtf8Printable(db.lastError().text()));
        return false;
    }
    return true;
}

bool SQLiteDataAccess::batchInsert(const QString& dbPath, HistoryGranularity gran,
                                    const std::vector<DownSampledSample>& samples) {
    if (samples.empty()) return true;                     // 边界:空批=no-op
    const uint64_t ts = monthStartFromDbPath(dbPath);
    if (ts == 0) {
        qWarning("SQLiteDataAccess: batchInsert cannot parse YYYYMM from dbPath: %s",
                 qUtf8Printable(dbPath));
        return false;
    }
    if (!m_connForPath.contains(dbPath)) {
        if (!openMonth(ts)) return false;
    }
    QSqlDatabase& db = m_connForPath[dbPath];
    const QString table = getTableName(ts, gran);
    if (table.isEmpty()) {
        qWarning("SQLiteDataAccess: batchInsert cannot resolve table name");
        return false;
    }
    if (!ensureSchema(dbPath, gran)) return false;       // 兜底:首次写入前建表

    enterWriteBatch();
    if (!db.transaction()) {
        qWarning("SQLiteDataAccess: BEGIN failed: %s",
                 qUtf8Printable(db.lastError().text()));
        leaveWriteBatch();
        return false;
    }
    const QString sql = QStringLiteral(
        "INSERT INTO %1 (point_id, ts, v_max, v_min, v_avg, sample_count) "
        "VALUES (?,?,?,?,?,?)").arg(table);
    QSqlQuery q(db);
    // Qt 5.15 QSqlQuery::prepare 返 QSqlQuery&;用 lastError 判定 + QVariant 显式包装 bind
    q.prepare(sql);
    if (q.lastError().isValid()) {
        qWarning("SQLiteDataAccess: PREPARE failed: %s",
                 qUtf8Printable(q.lastError().text()));
        db.rollback();
        leaveWriteBatch();
        return false;
    }
    for (const auto& s : samples) {
        q.addBindValue(QVariant(static_cast<qlonglong>(s.pointId)));
        q.addBindValue(QVariant(static_cast<qlonglong>(s.timestamp)));
        q.addBindValue(QVariant(static_cast<double>(s.maxValue)));
        q.addBindValue(QVariant(static_cast<double>(s.minValue)));
        q.addBindValue(QVariant(static_cast<double>(s.avgValue)));
        q.addBindValue(QVariant(static_cast<qlonglong>(s.sampleCount)));
        if (!q.exec()) {
            qWarning("SQLiteDataAccess: INSERT failed: %s",
                     qUtf8Printable(q.lastError().text()));
            db.rollback();
            leaveWriteBatch();
            return false;
        }
    }
    if (!db.commit()) {
        qWarning("SQLiteDataAccess: COMMIT failed: %s",
                 qUtf8Printable(db.lastError().text()));
        db.rollback();
        leaveWriteBatch();
        return false;
    }
    leaveWriteBatch();
    return true;
}

std::vector<DownSampledSample> SQLiteDataAccess::queryRange(uint32_t pointId,
                                                            uint64_t beginMs,
                                                            uint64_t endMs,
                                                            HistoryGranularity gran) {
    std::vector<DownSampledSample> out;
    if (beginMs >= endMs) return out;                       // 边界：空区间

    // 跨月路由：从 beginMs 所在月起逐月推进（每个月的库是独立 DB 文件）
    QDateTime cur = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(beginMs));
    cur.setDate(QDate(cur.date().year(), cur.date().month(), 1));
    cur.setTime(QTime(0, 0, 0));
    while (cur.toMSecsSinceEpoch() < static_cast<qint64>(endMs)) {
        const uint64_t monthStart = static_cast<uint64_t>(cur.toMSecsSinceEpoch());
        const QDateTime nextMonth = cur.addMonths(1);
        const uint64_t monthEnd   = static_cast<uint64_t>(nextMonth.toMSecsSinceEpoch());
        const uint64_t qBegin = std::max(beginMs, monthStart);
        const uint64_t qEnd   = std::min(endMs, monthEnd);

        if (qBegin < qEnd) {
            const QString table = getTableName(qBegin, gran);
            if (!table.isEmpty() && openMonth(qBegin)) {
                QSqlDatabase& db = m_connForPath[getDatabasePath(qBegin)];
                QSqlQuery q(db);
                q.prepare(QStringLiteral(
                    "SELECT ts, v_max, v_min, v_avg, sample_count FROM %1 "
                    "WHERE point_id=? AND ts>=? AND ts<? ORDER BY ts ASC").arg(table));
                q.addBindValue(QVariant(static_cast<qlonglong>(pointId)));
                q.addBindValue(QVariant(static_cast<qlonglong>(qBegin)));
                q.addBindValue(QVariant(static_cast<qlonglong>(qEnd)));
                q.exec();
                if (!q.lastError().isValid()) {
                    while (q.next()) {
                        DownSampledSample s;
                        s.pointId     = pointId;
                        s.timestamp   = static_cast<uint64_t>(q.value(0).toLongLong());
                        s.maxValue    = static_cast<float>(q.value(1).toDouble());
                        s.minValue    = static_cast<float>(q.value(2).toDouble());
                        s.avgValue    = static_cast<float>(q.value(3).toDouble());
                        s.sampleCount = static_cast<uint16_t>(q.value(4).toInt());
                        out.push_back(s);
                    }
                }
            }
        }
        cur = nextMonth;
    }
    return out;
}

// ─────────────────────────── 告警库（切片 35，DBDD §4.4）───────────────────────────

bool SQLiteDataAccess::openAlarmMonth(uint64_t timestamp) {
    if (timestamp == 0 || timestamp > kMaxSaneTs) {
        qWarning("SQLiteDataAccess: openAlarmMonth invalid timestamp %llu",
                 static_cast<unsigned long long>(timestamp));
        return false;
    }
    const QString dbPath = getAlarmDatabasePath(m_dataRootDir, timestamp);
    if (m_connForPath.contains(dbPath)) return true;     // 已打开

    const QString connName = makeConnName();
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
    db.setDatabaseName(dbPath);
    if (!db.open()) {
        qWarning("SQLiteDataAccess: openAlarmMonth failed: %s | err=%s",
                 qUtf8Printable(dbPath), qUtf8Printable(db.lastError().text()));
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
        return false;
    }
    if (!applyPragmas(db)) {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
        return false;
    }
    m_connForPath.insert(dbPath, db);
    if (!ensureAlarmSchema(dbPath)) {
        m_connForPath.remove(dbPath);
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
        return false;
    }
    return true;
}

bool SQLiteDataAccess::ensureAlarmSchema(const QString& dbPath) {
    if (!m_connForPath.contains(dbPath)) {
        // 无连接：先打开（openAlarmMonth 内部会再调本函数，届时已有连接 → 走建表分支）
        const uint64_t ts = alarmMonthFromDbPath(dbPath);
        if (ts == 0) {
            qWarning("SQLiteDataAccess: ensureAlarmSchema cannot parse YYYYMM from dbPath: %s",
                     qUtf8Printable(dbPath));
            return false;
        }
        return openAlarmMonth(ts);
    }
    QSqlDatabase& db = m_connForPath[dbPath];
    const uint64_t ts = alarmMonthFromDbPath(dbPath);
    if (ts == 0) {
        qWarning("SQLiteDataAccess: ensureAlarmSchema cannot parse YYYYMM from dbPath: %s",
                 qUtf8Printable(dbPath));
        return false;
    }
    const QString table = alarmTableNameOf(ts);
    const QString ddl = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS %1 ("
        "  id            INTEGER PRIMARY KEY,"
        "  point_id      INTEGER NOT NULL,"
        "  level         INTEGER NOT NULL,"
        "  status        INTEGER NOT NULL DEFAULT 0,"
        "  trigger_time  INTEGER NOT NULL,"
        "  recover_time  INTEGER NOT NULL DEFAULT 0,"
        "  confirm_user  TEXT,"
        "  confirm_time  INTEGER NOT NULL DEFAULT 0,"
        "  alarm_value   REAL    NOT NULL,"
        "  threshold     REAL    NOT NULL,"
        "  description   TEXT,"
        "  blackbox_id   INTEGER NOT NULL DEFAULT 0,"
        "  CONSTRAINT chk_level  CHECK (level  BETWEEN 0 AND 2),"
        "  CONSTRAINT chk_status CHECK (status BETWEEN 0 AND 2)"
        ")").arg(table);
    db.exec(ddl);
    if (db.lastError().isValid()) {
        qWarning("SQLiteDataAccess: CREATE TABLE %s failed: %s",
                 qUtf8Printable(table), qUtf8Printable(db.lastError().text()));
        return false;
    }
    const QStringList indexes = {
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_alarm_point_tr ON %1 (point_id, trigger_time)").arg(table),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_alarm_lv_st   ON %1 (level, status)").arg(table),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_alarm_trigger ON %1 (trigger_time)").arg(table),
    };
    for (const QString& idx : indexes) {
        db.exec(idx);
        if (db.lastError().isValid()) {
            qWarning("SQLiteDataAccess: CREATE INDEX on %s failed: %s",
                     qUtf8Printable(table), qUtf8Printable(db.lastError().text()));
            return false;
        }
    }
    return true;
}

bool SQLiteDataAccess::insertAlarmRecords(const std::vector<AlarmRecord>& records) {
    if (records.empty()) return true;                    // 边界：空批 = no-op
    const uint64_t ts = records.front().triggerTime;
    if (ts == 0 || ts > kMaxSaneTs) {
        qWarning("SQLiteDataAccess: insertAlarmRecords invalid triggerTime");
        return false;
    }
    const QString dbPath = getAlarmDatabasePath(m_dataRootDir, ts);
    if (!m_connForPath.contains(dbPath)) {
        if (!openAlarmMonth(ts)) return false;
    }
    QSqlDatabase& db = m_connForPath[dbPath];
    const QString table = alarmTableNameOf(ts);
    if (table.isEmpty() || !ensureAlarmSchema(dbPath)) return false;

    enterWriteBatch();
    if (!db.transaction()) {
        qWarning("SQLiteDataAccess: alarm BEGIN failed: %s",
                 qUtf8Printable(db.lastError().text()));
        leaveWriteBatch();
        return false;
    }
    const QString sql = QStringLiteral(
        "INSERT INTO %1 (id, point_id, level, status, trigger_time, recover_time,"
        " confirm_user, confirm_time, alarm_value, threshold, description, blackbox_id)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?)").arg(table);
    QSqlQuery q(db);
    q.prepare(sql);
    if (q.lastError().isValid()) {
        qWarning("SQLiteDataAccess: alarm PREPARE failed: %s",
                 qUtf8Printable(q.lastError().text()));
        db.rollback();
        leaveWriteBatch();
        return false;
    }
    for (const auto& r : records) {
        q.addBindValue(QVariant(static_cast<qlonglong>(r.id)));
        q.addBindValue(QVariant(static_cast<qlonglong>(r.pointId)));
        q.addBindValue(QVariant(r.level));
        q.addBindValue(QVariant(r.status));
        q.addBindValue(QVariant(static_cast<qlonglong>(r.triggerTime)));
        q.addBindValue(QVariant(static_cast<qlonglong>(r.recoverTime)));
        q.addBindValue(QVariant(r.confirmUser));
        q.addBindValue(QVariant(static_cast<qlonglong>(r.confirmTime)));
        q.addBindValue(QVariant(r.alarmValue));
        q.addBindValue(QVariant(r.threshold));
        q.addBindValue(QVariant(r.description));
        q.addBindValue(QVariant(static_cast<qlonglong>(r.blackboxId)));
        if (!q.exec()) {
            qWarning("SQLiteDataAccess: alarm INSERT failed: %s",
                     qUtf8Printable(q.lastError().text()));
            db.rollback();
            leaveWriteBatch();
            return false;
        }
    }
    if (!db.commit()) {
        qWarning("SQLiteDataAccess: alarm COMMIT failed: %s",
                 qUtf8Printable(db.lastError().text()));
        db.rollback();
        leaveWriteBatch();
        return false;
    }
    leaveWriteBatch();
    return true;
}

bool SQLiteDataAccess::setAlarmRecovered(uint64_t triggerTime, uint64_t alarmId,
                                         uint64_t recoverTime) {
    if (triggerTime == 0 || triggerTime > kMaxSaneTs) return false;
    const QString dbPath = getAlarmDatabasePath(m_dataRootDir, triggerTime);
    if (!m_connForPath.contains(dbPath)) {
        if (!openAlarmMonth(triggerTime)) return false;
    }
    QSqlDatabase& db = m_connForPath[dbPath];
    const QString table = alarmTableNameOf(triggerTime);
    if (table.isEmpty()) return false;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE %1 SET status=2, recover_time=? WHERE id=?").arg(table));
    q.addBindValue(QVariant(static_cast<qlonglong>(recoverTime)));
    q.addBindValue(QVariant(static_cast<qlonglong>(alarmId)));
    q.exec();
    if (q.lastError().isValid()) {
        qWarning("SQLiteDataAccess: alarm RECOVER update failed: %s",
                 qUtf8Printable(q.lastError().text()));
        return false;
    }
    return true;                                          // 无行命中（重启边界）幂等 true
}

bool SQLiteDataAccess::setAlarmConfirmed(uint64_t triggerTime, uint64_t alarmId,
                                         const QString& user, uint64_t confirmTime) {
    if (triggerTime == 0 || triggerTime > kMaxSaneTs) return false;
    const QString dbPath = getAlarmDatabasePath(m_dataRootDir, triggerTime);
    if (!m_connForPath.contains(dbPath)) {
        if (!openAlarmMonth(triggerTime)) return false;
    }
    QSqlDatabase& db = m_connForPath[dbPath];
    const QString table = alarmTableNameOf(triggerTime);
    if (table.isEmpty()) return false;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE %1 SET status=1, confirm_user=?, confirm_time=? "
        "WHERE id=? AND status=0").arg(table));
    q.addBindValue(QVariant(user));
    q.addBindValue(QVariant(static_cast<qlonglong>(confirmTime)));
    q.addBindValue(QVariant(static_cast<qlonglong>(alarmId)));
    q.exec();
    if (q.lastError().isValid()) {
        qWarning("SQLiteDataAccess: alarm CONFIRM update failed: %s",
                 qUtf8Printable(q.lastError().text()));
        return false;
    }
    return true;
}

}  // namespace ens::datahub
