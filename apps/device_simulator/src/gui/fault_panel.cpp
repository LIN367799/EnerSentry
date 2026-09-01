// src/gui/fault_panel.cpp —— B10 FaultPanel 实现（ENS-SIM-IMP §10.1/§10.3）
// FaultRequest{FaultOverride{type, scope, slave, reg, targetValue, rampRate,
//               recoverValue, corruptMs, durationMs}} —— 字段定义见 FaultInjector.h。
// CommLoss/CrcError/Timeout 属链路级故障：scope 建议 SLAVE/ALL，corruptMs 生效。
#include "gui/fault_panel.h"
#include "ui_fault_panel.h"

#include "sim/FaultInjector.h"
#include "sim/SimulatorEngine.h"

#include <QMessageBox>
#include <QRandomGenerator>

#include <random>

FaultPanel::FaultPanel(ens::sim::SimulatorEngine* engine, QWidget* parent)
    : QWidget(parent), ui(new Ui::FaultPanel), m_engine(engine) {
    ui->setupUi(this);

    connect(ui->btnInject, &QPushButton::clicked, this, &FaultPanel::onInjectClicked);
    connect(ui->btnRecoverAll, &QPushButton::clicked, this, &FaultPanel::onRecoverAllClicked);
    connect(&m_periodic, &QTimer::timeout, this, &FaultPanel::onPeriodicTick);
    // 作用域联动：ALL 时从站/寄存器置灰
    connect(ui->comboScope, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        const bool needSlave = (idx != 0);            // 非 ALL
        const bool needPoint = (idx == 2);            // POINT
        ui->spinSlave->setEnabled(needSlave);
        ui->spinReg->setEnabled(needPoint);
    });
    ui->spinSlave->setEnabled(false);
    ui->spinReg->setEnabled(false);
}

FaultPanel::~FaultPanel() {
    m_periodic.stop();
    delete ui;
}

void FaultPanel::setEngineRunning(bool running) {
    m_engineRunning = running;
    m_periodic.stop();
    m_periodicArmed = false;
    if (!running) {
        ui->btnInject->setEnabled(false);
        ui->btnRecoverAll->setEnabled(false);
        appendStatus(QStringLiteral("引擎已停止：注入/恢复按钮禁用"));
    } else {
        ui->btnInject->setEnabled(true);
        ui->btnRecoverAll->setEnabled(true);
        appendStatus(QStringLiteral("引擎运行中，故障注入就绪"));
    }
}

ens::sim::FaultRequest FaultPanel::makeRequest() {
    using ens::sim::FaultType;
    using ens::sim::Scope;

    ens::sim::FaultRequest req;
    req.spec.type = static_cast<FaultType>(ui->comboType->currentIndex());
    req.spec.scope = static_cast<Scope>(ui->comboScope->currentIndex());

    // ALL 作用域：slave/reg 无意义（引擎侧按 ALL 通配）；SLAVE/POINT 需 slave
    req.spec.slave = static_cast<uint8_t>(ui->spinSlave->value());
    req.spec.reg   = static_cast<uint16_t>(ui->spinReg->value());

    req.spec.targetValue  = static_cast<float>(ui->spinTarget->value());
    req.spec.rampRate     = static_cast<float>(ui->spinRamp->value());
    req.spec.durationMs   = ui->spinDuration->value();
    req.spec.corruptMs    = ui->spinDuration->value();  // 链路级故障按持续时长延迟/破坏

    // 随机模式：目标值在 [0, 目标值×2] 区间抖动（演示用，保持可观测）
    if (ui->comboMode->currentIndex() == 2) {
        const float base = static_cast<float>(ui->spinTarget->value());
        const float v = base * static_cast<float>(QRandomGenerator::global()->generateDouble() * 2.0);
        req.spec.targetValue = v;
    }
    return req;
}

void FaultPanel::onInjectClicked() {
    if (!m_engine || !m_engineRunning) return;
    const auto req = makeRequest();
    const uint32_t h = m_engine->injectFault(req);
    if (h == ens::sim::INVALID_FAULT_HANDLE) {
        appendStatus(QStringLiteral("注入失败（handle 耗尽或引擎未就绪）"));
        return;
    }
    m_handles.push_back(h);
    const QString scope = ui->comboScope->currentText();
    const QString type  = ui->comboType->currentText();
    appendStatus(QStringLiteral("已注入 #%1  %2 / %3（slave=%4 reg=%5 目标=%6℃）")
                     .arg(h).arg(type, scope)
                     .arg(ui->spinSlave->value())
                     .arg(ui->spinReg->value())
                     .arg(ui->spinTarget->value()));

    // 周期模式：间隔 spinInterval(ms) 重复注入（每次新 handle）
    if (ui->comboMode->currentIndex() == 1) {
        m_periodic.setInterval(ui->spinInterval->value());
        m_periodic.start();
        m_periodicArmed = true;
    }
}

void FaultPanel::onPeriodicTick() {
    if (!m_engine || !m_engineRunning) { m_periodic.stop(); m_periodicArmed = false; return; }
    const auto req = makeRequest();
    const uint32_t h = m_engine->injectFault(req);
    if (h != ens::sim::INVALID_FAULT_HANDLE) {
        m_handles.push_back(h);
        ++m_periodicCount;
        appendStatus(QStringLiteral("周期注入 #%1（第 %2 次）").arg(h).arg(m_periodicCount));
    }
}

void FaultPanel::onRecoverAllClicked() {
    if (!m_engine || !m_engineRunning) return;
    m_periodic.stop();
    m_periodicArmed = false;
    int ok = 0;
    for (uint32_t h : m_handles) {
        if (m_engine->recoverFault(h)) ++ok;
    }
    appendStatus(QStringLiteral("全部恢复：%1/%2 个会话进入恢复").arg(ok).arg(m_handles.size()));
    m_handles.clear();
    m_periodicCount = 0;
}

void FaultPanel::appendStatus(const QString& msg) {
    ui->lblStatus->setText(msg);
}
