// src/gui/main_window.h —— B10 SimulatorMainWindow（ENS-SIM-IMP §10.1/§10.4）
// 主窗口：菜单/工具栏（启动·停止·重载）/状态栏（监听地址:端口、运行态、tickCount）/
// 中央 QTabWidget 四页（寄存器/故障/场景/日志）+ 内嵌运行配置面板（SIM-IMP §10.1 config_panel）。
// 生命周期：aboutToQuit → engine.stop() 优雅停（DataTick 线程 join）。
// 刷新单向：GUI 只读引擎快照/接口，绝不回写热路径（ADR-22）。
#pragma once

#include <QHash>
#include <QLabel>
#include <QMainWindow>
#include <QTimer>

#include "sim/sim_config.h"

namespace ens::sim {
class SimulatorEngine;
}  // namespace ens::sim

namespace Ui {
class MainWindow;
}

class RegisterView;
class FaultPanel;
class ScenarioRunner;
class LogView;

class SimulatorMainWindow : public QMainWindow {
    Q_OBJECT
public:
    SimulatorMainWindow(ens::sim::SimulatorEngine* engine, const ens::sim::SimConfig& cfg,
                        QWidget* parent = nullptr);
    ~SimulatorMainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onStartEngine();
    void onStopEngine();
    void onApplyConfig();
    void onAbout();
    void onStatusTick();
    void onTabChanged(int idx);

private:
    void setupIcons();
    bool ensureEngineStarted();

    Ui::MainWindow* ui;
    ens::sim::SimulatorEngine* m_engine;
    ens::sim::SimConfig  m_cfg;          // 副本：应用重启时更新后 stop+start

    RegisterView*   m_registerView = nullptr;
    FaultPanel*     m_faultPanel   = nullptr;
    ScenarioRunner* m_scenarioRunner = nullptr;
    LogView*        m_logView      = nullptr;

    QLabel* m_lblLink = nullptr;   // 状态栏：监听地址:端口
    QLabel* m_lblRun  = nullptr;   // 状态栏：运行态
    QLabel* m_lblTick = nullptr;   // 状态栏：tickCount
    QTimer  m_statusTimer;         // 1s 状态刷新
    bool    m_running = false;
};
