// src/gui/log_view.h —— B10 LogView（ENS-SIM-IMP §10.1）
// 事件日志：1s 增量轮询 SimulatorEngine::scenarioEventsJsonl()（引擎零 Qt 信号，R2 设计），
// 按事件关键字着色（FAULT 红 / RECOVER 绿 / INFO 蓝）；级别筛选；导出 .txt。
#pragma once

#include <QStringList>
#include <QTimer>
#include <QWidget>

namespace ens::sim {
class SimulatorEngine;
}  // namespace ens::sim

namespace Ui {
class LogView;
}

class LogView : public QWidget {
    Q_OBJECT
public:
    explicit LogView(ens::sim::SimulatorEngine* engine, QWidget* parent = nullptr);
    ~LogView() override;

    void setEngineRunning(bool running);

private slots:
    void onPollTick();
    void onLevelFilterChanged();
    void onClearClicked();
    void onExportClicked();

private:
    void repaintFiltered();

    Ui::LogView* ui;
    ens::sim::SimulatorEngine* m_engine;
    QTimer m_timer;          // 1s 轮询
    QStringList m_lines;     // 全部事件行（原始）
    int      m_lastCount = 0;
    bool     m_engineRunning = false;
};
