// src/ui/views/alarm_center_widget.h —— L5 告警中心（切片 36 完整版，FR-AL-11/FR-EXP-03）。
// 数据模型：历史查询（IAlarmAccess 抽象注入，UI 禁触 SQLite）+ 实时事件防抖重查。
// 功能：① 筛选查询（级别/状态/测点/时间范围，FR-AL-11）② 确认（选中行 → engine ack，
//       操作人来自 currentUser 回调）③ 导出 CSV（CsvWriter 全审计字段，FR-EXP-03）。
// 空指针全容忍：alarm/access/pt/auth 任一为空 → 对应功能禁用，构造不崩（ui_smoke 全 null 源）。
#pragma once

#include <QStandardItemModel>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>
#include <vector>

#include "AlarmEntities.h"          // AlarmEvent
#include "IAlarmAccess.h"           // AlarmRecord / AlarmQueryFilter

namespace ens::business {
class AlarmEngine;
}  // namespace ens::business

namespace ens::protocol {
class PointTable;
}  // namespace ens::protocol

namespace ens::datahub {
class IAlarmAccess;
class IL1SnapshotReader;
}  // namespace ens::datahub

namespace Ui {
class AlarmCenterWidget;
}

namespace ens::ui {

class AlarmCenterWidget : public QWidget {
    Q_OBJECT
public:
    /// @param alarm        告警引擎（实时事件 + 确认；可为 null → 实时/确认禁用）
    /// @param access       告警历史查询抽象（可为 null → 表格只读空态）
    /// @param pt           点表（测点下拉 + 名称列；可为 null → 名称列空）
    /// @param currentUser  当前登录用户回调（确认人；返回空 → 确认按钮禁用）
    /// @param replay       L1 高频快照回放读取（FR-AL-12；可为 null → 回放按钮禁用）
    explicit AlarmCenterWidget(ens::business::AlarmEngine* alarm,
                               ens::datahub::IAlarmAccess* access,
                               const std::shared_ptr<ens::protocol::PointTable>& pt,
                               std::function<QString()> currentUser = {},
                               ens::datahub::IL1SnapshotReader* replay = nullptr,
                               QWidget* parent = nullptr);
    ~AlarmCenterWidget() override;

private slots:
    void onQueryClicked();
    void onConfirmClicked();
    void onExportClicked();
    void onReplayClicked();                                      // 切片 38：FR-AL-12
    void onAlarmTriggered(const ens::business::AlarmEvent& ev);   // → 1s 防抖重查
    void onRefreshActive();                                        // 1s 活跃计数

private:
    void fillPoints(const std::shared_ptr<ens::protocol::PointTable>& pt);
    void fillLevelsAndStatus();
    /// 按当前控件状态组查询过滤器（范围 combo data = ms；index0=1h 等）
    ens::datahub::AlarmQueryFilter currentFilter() const;
    void reload(const std::vector<ens::datahub::AlarmRecord>& rows);
    void applyRows(const std::vector<ens::datahub::AlarmRecord>& rows);
    /// 行号 → 告警 id（item UserRole）
    static uint64_t alarmIdOf(const QStandardItemModel* model, int row);

    Ui::AlarmCenterWidget* ui;
    ens::business::AlarmEngine* m_alarm   = nullptr;   // 不拥有
    ens::datahub::IAlarmAccess* m_access  = nullptr;   // 不拥有
    ens::datahub::IL1SnapshotReader* m_replay = nullptr;   // 不拥有（切片 38）
    std::shared_ptr<ens::protocol::PointTable> m_pt;
    std::function<QString()> m_currentUser;

    QStandardItemModel* m_model = nullptr;
    QTimer m_activeTimer;                // 1s 活跃计数
    QTimer m_debounceTimer;              // 1s 单发：alarmTriggered → 重查
    std::vector<ens::datahub::AlarmRecord> m_lastRows;   // 导出数据源（当前查询结果）
};

}  // namespace ens::ui
