// src/datahub/SQLiteDataAccess.h
// L3 数据中枢 ── SQLite 数据访问层（ENS-LLD-200 §4 / DBDD §4-§5）。
//
// 关键设计：
//   * 按月分库:data_YYYYMM.db,按测点 pointId 与粒度 gran 路由(history_<gran>_YYYYMM)
//   * PRAGMA 调优:journal_mode=WAL, synchronous=NORMAL, cache_size=64MB, mmap=256MB, busy_timeout=3s
//   * ensureSchema:首次打开月库时建表(WITHOUT ROWID 主键,见 §4.1.1)
//   * batchInsert:事务内 prepared stmt + bind,失败回滚
//   * 跨月 ATTACH(LLD §4.4):本切片简化,留 TODO;V.x 修订
//   * 写入通知 enterWriteBatch/leaveWriteBatch:本切片简化,留 TODO;V.x 修订

#pragma once

#include "DownSampler.h"

#include <cstdint>
#include <vector>

#include <QHash>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

namespace ens::datahub {

/// SQLite 数据访问层(Q_OBJECT 以便 QObject 派生语义,虽本切片不主动用 signal/slot)
class SQLiteDataAccess : public QObject {
    Q_OBJECT
public:
    /// @param dataRootDir 月库根目录(各月子目录 <root>/history/YYYYMM/data_YYYYMM.db)
    explicit SQLiteDataAccess(const QString& dataRootDir, QObject* parent = nullptr);
    ~SQLiteDataAccess() override;

    SQLiteDataAccess(const SQLiteDataAccess&) = delete;
    SQLiteDataAccess& operator=(const SQLiteDataAccess&) = delete;
    SQLiteDataAccess(SQLiteDataAccess&&) = delete;
    SQLiteDataAccess& operator=(SQLiteDataAccess&&) = delete;

    // ── 表名/路径路由(LLD-200 §4.3) ──
    /// 粒度 → 表名后缀(_100ms/_1s/_5s/_1m)
    static QString granularitySuffix(HistoryGranularity gran);
    /// 时间戳 → 表名(history_<gran>_YYYYMM);非法时间戳返空
    QString getTableName(uint64_t timestamp, HistoryGranularity gran) const;
    /// 时间戳 → 单月 DB 完整路径(<root>/history/YYYYMM/data_YYYYMM.db)
    QString getDatabasePath(uint64_t timestamp) const;
    /// 显式月库路径(告警/审计独立库,本切片预留接口)
    static QString getAlarmDatabasePath(const QString& root, uint64_t timestamp);

    // ── 连接管理 ──
    /// 获取或打开某月库的连接(首次打开时应用 PRAGMA + ensureSchema)
    /// @return true 成功;false 打开失败(目录权限/磁盘满等)
    bool openMonth(uint64_t timestamp);

    /// 关闭所有打开的连接(析构时自动调用)
    void closeAll();

    /// 诊断:已打开连接数
    int openConnectionCount() const { return static_cast<int>(m_connForPath.size()); }

    // ── Schema(LLD-200 §4.1.1 DDL) ──
    /// 建表 history_<gran>_YYYYMM(WITHOUT ROWID 主键);粒度表已存在则跳过
    bool ensureSchema(const QString& dbPath, HistoryGranularity gran);

    // ── 批量写入(LLD-200 §4.5 事务) ──
    /// 单月批量 INSERT;整批一个事务,失败回滚
    /// @return true 全部成功;false 任一失败(事务已回滚)
    bool batchInsert(const QString& dbPath, HistoryGranularity gran,
                     const std::vector<DownSampledSample>& samples);

    // ── 写入通知(LLD-200 §4.4.2 写事务避让)— V.x 简化 ──
    /// 简化版本:仅记录标志,不做 condvar 通知(等切片 9/10 补齐)
    void enterWriteBatch() { m_isWritingBatch.store(true, std::memory_order_release); }
    void leaveWriteBatch() { m_isWritingBatch.store(false, std::memory_order_release); }
    bool isWritingBatch() const { return m_isWritingBatch.load(std::memory_order_acquire); }

private:
    QString makeConnName();                                     // 唯一连接名生成
    bool applyPragmas(QSqlDatabase& db);                         // §4.2 PRAGMA 调优

    QString m_dataRootDir;
    QHash<QString, QSqlDatabase> m_connForPath;                 // dbPath → QSqlDatabase
    std::atomic<bool> m_isWritingBatch{false};
    int m_nextConnIdx = 0;                                      // 连接名自增
};

}  // namespace ens::datahub
