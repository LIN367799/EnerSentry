// src/gui/fault_panel.h —— B10 FaultPanel（ENS-SIM-IMP §10.1）
// 故障注入控制台：类型 5a~5e / 作用域 ALL·SLAVE·POINT / 触发模式 单次·周期·随机
// → SimulatorEngine::injectFault()（线程安全）。「全部恢复」遍历本地 handle 表 recoverFault。
// 脚本模式归 ScenarioRunner（本面板禁用，见 .ui 提示）。
#pragma once

#include <QTimer>
#include <QVector>
#include <QWidget>
#include <cstdint>

namespace ens::sim {
class SimulatorEngine;
struct FaultRequest;
}  // namespace ens::sim

namespace Ui {
class FaultPanel;
}

class FaultPanel : public QWidget {
    Q_OBJECT
public:
    explicit FaultPanel(ens::sim::SimulatorEngine* engine, QWidget* parent = nullptr);
    ~FaultPanel() override;

    /// MainWindow 引擎启停时调用：停止后禁用注入（引擎未运行时 injectFault 无效）
    void setEngineRunning(bool running);

private slots:
    void onInjectClicked();
    void onRecoverAllClicked();
    void onPeriodicTick();

private:
    ens::sim::FaultRequest makeRequest();   // 由当前表单组 FaultRequest
    void appendStatus(const QString& msg);

    Ui::FaultPanel* ui;
    ens::sim::SimulatorEngine* m_engine;
    QVector<uint32_t> m_handles;   // 本面板已注入的 handle（全部恢复遍历用）
    QTimer m_periodic;
    bool   m_engineRunning = false;
    bool   m_periodicArmed = false;
    uint32_t m_periodicCount = 0;
};
