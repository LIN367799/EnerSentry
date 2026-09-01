// src/gui/scenario_runner.cpp —— B10 ScenarioRunner 实现（ENS-SIM-IMP §10.1/§10.3）
// drill JSON schema（ENS-SIM-IMP §7）：{"name","steps":[{t,action,fault,scope,...}]}
// 进度推进：elapsedMs = engine.tickCount() * engine.tickMs()（引擎单调 tick 时钟，
// 与场景驱动同一时钟源，t 字段即该时钟下的毫秒）。
#include "gui/scenario_runner.h"
#include "ui_scenario_runner.h"

#include "sim/SimulatorEngine.h"

#include <QFile>
#include <QFileDialog>
#include <QMessageBox>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

ScenarioRunner::ScenarioRunner(ens::sim::SimulatorEngine* engine, QWidget* parent)
    : QWidget(parent), ui(new Ui::ScenarioRunner), m_engine(engine) {
    ui->setupUi(this);

    connect(ui->btnOpen, &QPushButton::clicked, this, &ScenarioRunner::onOpenClicked);
    connect(ui->btnLoad, &QPushButton::clicked, this, &ScenarioRunner::onLoadClicked);
    connect(&m_timer, &QTimer::timeout, this, &ScenarioRunner::onProgressTick);

    // 进度条空闲态：0~100 常规范围，无步骤时保持 0
    ui->progress->setRange(0, 100);
}

ScenarioRunner::~ScenarioRunner() {
    m_timer.stop();
    delete ui;
}

void ScenarioRunner::setEngineRunning(bool running) {
    m_engineRunning = running;
    if (!running) {
        m_timer.stop();
        setStateText(QStringLiteral("引擎已停止"));
        m_completed = false;
    }
}

void ScenarioRunner::onOpenClicked() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开场景脚本"), QString(),
        QStringLiteral("drill JSON (*.json);;所有文件 (*)"));
    if (path.isEmpty()) return;

    // 解析 steps 总数 + 总时长（max over t）
    int steps = 0;
    int64_t totalMs = 0;
    try {
        std::ifstream ifs(path.toStdString());
        const nlohmann::json root = nlohmann::json::parse(ifs);
        if (root.contains("steps") && root["steps"].is_array()) {
            steps = static_cast<int>(root["steps"].size());
            for (const auto& st : root["steps"]) {
                if (st.contains("t") && st["t"].is_number()) {
                    totalMs = std::max<int64_t>(totalMs, st["t"].get<int64_t>());
                }
            }
        }
        if (steps == 0) {
            QMessageBox::warning(this, QStringLiteral("场景脚本"),
                                 QStringLiteral("JSON 中未找到 steps[] 数组"));
            return;
        }
    } catch (const std::exception& e) {
        QMessageBox::warning(this, QStringLiteral("场景脚本"),
                             QStringLiteral("解析失败：%1").arg(QString::fromLocal8Bit(e.what())));
        return;
    }

    m_path = path;
    m_stepCount = steps;
    m_totalMs = totalMs;
    m_completed = false;
    ui->editPath->setText(path);
    ui->btnLoad->setEnabled(true);
    ui->progress->setValue(0);
    setStateText(QStringLiteral("已解析：%1 步 / 总时长 %2 ms（点击「加载并启动」）")
                     .arg(steps).arg(totalMs));
}

void ScenarioRunner::onLoadClicked() {
    if (!m_engine || !m_engineRunning) {
        QMessageBox::warning(this, QStringLiteral("场景运行"),
                             QStringLiteral("引擎未运行，请先启动引擎"));
        return;
    }
    if (!m_engine->loadScenario(m_path.toStdString())) {
        QMessageBox::warning(this, QStringLiteral("场景运行"),
                             QStringLiteral("加载场景失败：%1").arg(m_path));
        return;
    }
    m_completed = false;
    ui->progress->setValue(0);
    setStateText(QStringLiteral("运行中… 已加载 %1 步").arg(m_stepCount));
    m_timer.start(100);
}

void ScenarioRunner::onProgressTick() {
    if (!m_engine || !m_engineRunning) { m_timer.stop(); return; }

    if (!m_engine->scenarioLoaded()) {
        setStateText(QStringLiteral("未加载场景"));
        m_timer.stop();
        return;
    }
    if (m_engine->scenarioAllFired()) {
        if (!m_completed) {
            m_completed = true;
            ui->progress->setValue(100);
            setStateText(QStringLiteral("已完成：全部 step 已触发（事件见「事件日志」页）"));
        }
        m_timer.stop();
        return;
    }

    // 进度 = 已推进时间 / 总时长
    const int64_t elapsed = static_cast<int64_t>(m_engine->tickCount()) * m_engine->tickMs();
    int pct = (m_totalMs > 0) ? static_cast<int>(elapsed * 100 / m_totalMs) : 0;
    pct = std::clamp(pct, 0, 99);
    ui->progress->setValue(pct);
    setStateText(QStringLiteral("运行中… %1 ms / %2 ms（%3%）")
                     .arg(elapsed).arg(m_totalMs).arg(pct));
}

void ScenarioRunner::setStateText(const QString& t) {
    ui->lblState->setText(t);
}
