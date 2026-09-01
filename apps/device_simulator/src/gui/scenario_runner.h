// src/gui/scenario_runner.h —— B10 ScenarioRunner（ENS-SIM-IMP §10.1）
// 场景脚本运行器：打开 drill JSON → 解析 steps 数/总时长 → SimulatorEngine::loadScenario()。
// 进度 = 引擎 tickCount×tickMs 时间推进（GUI 侧纯计算，零引擎新接口）。
// 场景为一次性驱动（B9 设计），无暂停/停止接口——结束后重新加载可重跑。
#pragma once

#include <QTimer>
#include <QWidget>
#include <cstdint>

namespace ens::sim {
class SimulatorEngine;
}  // namespace ens::sim

namespace Ui {
class ScenarioRunner;
}

class ScenarioRunner : public QWidget {
    Q_OBJECT
public:
    explicit ScenarioRunner(ens::sim::SimulatorEngine* engine, QWidget* parent = nullptr);
    ~ScenarioRunner() override;

    void setEngineRunning(bool running);

private slots:
    void onOpenClicked();
    void onLoadClicked();
    void onProgressTick();

private:
    void setStateText(const QString& t);

    Ui::ScenarioRunner* ui;
    ens::sim::SimulatorEngine* m_engine;

    QString   m_path;
    int       m_stepCount   = 0;
    int64_t   m_totalMs     = 0;
    bool      m_engineRunning = false;
    QTimer    m_timer;          // 100ms 进度轮询
    bool      m_completed   = false;
};
