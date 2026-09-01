// src/gui/register_view.h —— B10 RegisterView（ENS-SIM-IMP §10.1）
// 设备树（23 从站分组）+ 实时寄存器表；30Hz QTimer → RegisterBank::snapshot() 只读轮询。
// 刷新单向：GUI 只读快照，绝不回写引擎（ADR-22 对齐）。名称/告警双过滤。
#pragma once

#include <QHash>
#include <QStandardItemModel>
#include <QTimer>
#include <QVector>
#include <QWidget>

namespace ens::sim {
class SimulatorEngine;
class SimPoint;
class SlaveRegset;
}  // namespace ens::sim

namespace Ui {
class RegisterView;
}

class RegisterView : public QWidget {
    Q_OBJECT
public:
    explicit RegisterView(ens::sim::SimulatorEngine* engine, QWidget* parent = nullptr);
    ~RegisterView() override;

    /// MainWindow 引擎启停时调用：未运行时挂起 30Hz 轮询（防空表闪烁）
    void setEngineRunning(bool running);

private slots:
    void onRefreshTick();
    void onNameFilterChanged();
    void onAlarmFilterChanged();
    void onTreeClicked(const QModelIndex& idx);

private:
    void rebuildTree();
    void rebuildPointRows();
    void refreshTable();

    Ui::RegisterView* ui;
    ens::sim::SimulatorEngine* m_engine;

    QTimer            m_timer;         // 30Hz (33ms)
    QStandardItemModel* m_treeModel;
    QStandardItemModel* m_tableModel;

    QHash<uint8_t, int> m_slaveTreeRow;          // slaveId → 树行号
    QVector<const ens::sim::SimPoint*> m_points; // 当前表行 → 点（过滤后）
    QVector<uint8_t>    m_pointSlaves;           // 与 m_points 并行：所属 slave（取快照用）

    bool        m_engineRunning = false;
    uint8_t     m_slaveFilter   = 0;             // 0 = 全部
    bool        m_refreshing    = false;         // 过滤触发的表重建，避免与 30Hz 竞争
};
