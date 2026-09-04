// src/gui/main_window.cpp —— B10 SimulatorMainWindow 实现（ENS-SIM-IMP §10.1~§10.4）
// 图标资源前缀 :/icons/*.svg（resources.qrc，Feather v4.29.2 MIT + 自绘 Logo）。
// 引擎启停通过工具栏/菜单动作；应用重启 = stop() + 更新 cfg + start()（start/stop 可重入）。
#include "gui/main_window.h"
#include "ui_main_window.h"

#include "gui/fault_panel.h"
#include "gui/log_view.h"
#include "gui/register_view.h"
#include "gui/scenario_runner.h"

#include "sim/SimulatorEngine.h"

#include <QCloseEvent>
#include <QIcon>
#include <QMessageBox>

SimulatorMainWindow::SimulatorMainWindow(ens::sim::SimulatorEngine* engine,
                                         const ens::sim::SimConfig& cfg, QWidget* parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_engine(engine), m_cfg(cfg) {
    ui->setupUi(this);

    // ── 装配四页（.ui 中 tab 为占位空页，子页面用代码 addWidget 注入）──
    m_registerView   = new RegisterView(m_engine, this);
    m_faultPanel     = new FaultPanel(m_engine, this);
    m_scenarioRunner = new ScenarioRunner(m_engine, this);
    m_logView        = new LogView(m_engine, this);
    ui->tabMain->removeTab(0);
    ui->tabMain->removeTab(0);
    ui->tabMain->removeTab(0);
    ui->tabMain->removeTab(0);
    ui->tabMain->addTab(m_registerView,   QIcon(QStringLiteral(":/icons/view_diag.svg")),  QStringLiteral("寄存器监视"));
    ui->tabMain->addTab(m_faultPanel,     QIcon(QStringLiteral(":/icons/sbo_estop.svg")), QStringLiteral("故障注入"));
    ui->tabMain->addTab(m_scenarioRunner, QIcon(QStringLiteral(":/icons/plot_play.svg")), QStringLiteral("场景运行"));
    ui->tabMain->addTab(m_logView,        QIcon(QStringLiteral(":/icons/sim_log.svg")),   QStringLiteral("事件日志"));

    setupIcons();
    connect(ui->tabMain, &QTabWidget::currentChanged, this, &SimulatorMainWindow::onTabChanged);

    // ── 动作 ──
    connect(ui->actStart,  &QAction::triggered, this, &SimulatorMainWindow::onStartEngine);
    connect(ui->actStop,   &QAction::triggered, this, &SimulatorMainWindow::onStopEngine);
    connect(ui->actReload, &QAction::triggered, this, &SimulatorMainWindow::onApplyConfig);
    connect(ui->actExit,   &QAction::triggered, this, &SimulatorMainWindow::close);
    connect(ui->actAbout,  &QAction::triggered, this, &SimulatorMainWindow::onAbout);
    connect(ui->btnApply,  &QPushButton::clicked, this, &SimulatorMainWindow::onApplyConfig);

    // ── 配置面板初始值 ──
    ui->spinTcpPort->setValue(m_cfg.tcp.port);
    ui->spinTickMs->setValue(static_cast<int>(m_cfg.tickMs));
    ui->spinSeed->setValue(static_cast<int>(m_cfg.seed));
    ui->checkRtu->setChecked(m_cfg.rtu.enabled);

    // ── 状态栏 ──
    m_lblLink = new QLabel(QStringLiteral("TCP %1:%2").arg(QString::fromStdString(m_cfg.tcp.bindIp))
                                                       .arg(m_cfg.tcp.port), this);
    m_lblRun  = new QLabel(QStringLiteral("运行态：停止"), this);
    m_lblTick = new QLabel(QStringLiteral("tick: 0"), this);
    statusBar()->addWidget(m_lblLink);
    statusBar()->addPermanentWidget(m_lblRun);
    statusBar()->addPermanentWidget(m_lblTick);

    connect(&m_statusTimer, &QTimer::timeout, this, &SimulatorMainWindow::onStatusTick);
    m_statusTimer.start(1000);

    // 引擎初始启动（GUI 模式默认直接跑起来，用户可停）
    onStartEngine();
}

SimulatorMainWindow::~SimulatorMainWindow() {
    m_statusTimer.stop();
    onStopEngine();   // 兜底：确保 DataTick 线程 join（closeEvent 已处理，析构再保险一次）
    delete ui;
}

void SimulatorMainWindow::setupIcons() {
    ui->actStart->setIcon(QIcon(QStringLiteral(":/icons/sim_start.svg")));
    ui->actStop->setIcon(QIcon(QStringLiteral(":/icons/sim_stop.svg")));
    ui->actReload->setIcon(QIcon(QStringLiteral(":/icons/sim_reload.svg")));
    ui->actExit->setIcon(QIcon(QStringLiteral(":/icons/menu_quit.svg")));
    ui->actAbout->setIcon(QIcon(QStringLiteral(":/icons/menu_about.svg")));
    ui->actTabRegister->setIcon(QIcon(QStringLiteral(":/icons/view_diag.svg")));
    ui->actTabFault->setIcon(QIcon(QStringLiteral(":/icons/sbo_estop.svg")));
    ui->actTabScenario->setIcon(QIcon(QStringLiteral(":/icons/plot_play.svg")));
    ui->actTabLog->setIcon(QIcon(QStringLiteral(":/icons/sim_log.svg")));
    ui->btnApply->setIcon(QIcon(QStringLiteral(":/icons/sim_reload.svg")));
    setWindowIcon(QIcon(QStringLiteral(":/icons/app_logo.svg")));

    // 视图菜单动作 checkable，与 tab 联动
    ui->actTabRegister->setCheckable(true);
    ui->actTabFault->setCheckable(true);
    ui->actTabScenario->setCheckable(true);
    ui->actTabLog->setCheckable(true);
    ui->actTabRegister->setChecked(true);
    // 视图动作触发 → 切 tab
    connect(ui->actTabRegister, &QAction::triggered, this, [this] { ui->tabMain->setCurrentIndex(0); });
    connect(ui->actTabFault,    &QAction::triggered, this, [this] { ui->tabMain->setCurrentIndex(1); });
    connect(ui->actTabScenario, &QAction::triggered, this, [this] { ui->tabMain->setCurrentIndex(2); });
    connect(ui->actTabLog,      &QAction::triggered, this, [this] { ui->tabMain->setCurrentIndex(3); });
}

bool SimulatorMainWindow::ensureEngineStarted() {
    if (m_running) return true;
    if (!m_engine->start(m_cfg)) {
        // 切片 43：透出引擎真实失败原因（lastError），替代笼统兜底文案，便于现场排错
        const QString reason = QString::fromStdString(m_engine->lastError());
        QMessageBox::critical(this, QStringLiteral("引擎"),
                              reason.isEmpty()
                                  ? QStringLiteral("SimulatorEngine 启动失败（点表路径或端口冲突）")
                                  : QStringLiteral("SimulatorEngine 启动失败：%1").arg(reason));
        return false;
    }
    m_running = true;
    ui->actStart->setEnabled(false);
    ui->actStop->setEnabled(true);
    m_registerView->setEngineRunning(true);
    m_faultPanel->setEngineRunning(true);
    m_scenarioRunner->setEngineRunning(true);
    m_logView->setEngineRunning(true);
    m_lblRun->setText(QStringLiteral("运行态：运行中"));
    return true;
}

void SimulatorMainWindow::onStartEngine() {
    ensureEngineStarted();
}

void SimulatorMainWindow::onStopEngine() {
    if (!m_running) return;
    m_engine->stop();
    m_running = false;
    ui->actStart->setEnabled(true);
    ui->actStop->setEnabled(false);
    m_registerView->setEngineRunning(false);
    m_faultPanel->setEngineRunning(false);
    m_scenarioRunner->setEngineRunning(false);
    m_logView->setEngineRunning(false);
    m_lblRun->setText(QStringLiteral("运行态：停止"));
}

void SimulatorMainWindow::onApplyConfig() {
    const bool wasRunning = m_running;
    onStopEngine();
    m_cfg.tcp.port     = static_cast<uint16_t>(ui->spinTcpPort->value());
    m_cfg.tickMs       = static_cast<uint32_t>(ui->spinTickMs->value());
    m_cfg.seed         = static_cast<uint32_t>(ui->spinSeed->value());
    m_cfg.rtu.enabled  = ui->checkRtu->isChecked();
    m_lblLink->setText(QStringLiteral("TCP %1:%2").arg(QString::fromStdString(m_cfg.tcp.bindIp))
                                                   .arg(m_cfg.tcp.port));
    if (wasRunning) {
        if (!ensureEngineStarted()) return;
        statusBar()->showMessage(QStringLiteral("配置已应用并重启引擎"), 3000);
    } else {
        statusBar()->showMessage(QStringLiteral("配置已更新（引擎未运行）"), 3000);
    }
}

void SimulatorMainWindow::onAbout() {
    QMessageBox::about(this, QStringLiteral("关于 EnerSentry 设备模拟器"),
        QStringLiteral(
            "<b>EnerSentry 设备模拟器控制台</b><br>"
            "B10 GUI 控制台（Phase 4 切片 18）<br><br>"
            "引擎：SimulatorEngine（DataTick + PointGenerator + FaultInjector + ModbusSlaveEmulator）<br>"
            "图标：Feather Icons v4.29.2 (MIT) + 自绘储能 Logo"));
}

void SimulatorMainWindow::onStatusTick() {
    if (!m_engine) return;
    m_lblTick->setText(QStringLiteral("tick: %1").arg(m_engine->tickCount()));
    if (m_engine->isRunning() != m_running) {
        // 外部停止（异常）同步按钮态
        if (!m_engine->isRunning() && m_running) onStopEngine();
    }
}

void SimulatorMainWindow::onTabChanged(int idx) {
    // 菜单「视图」动作联动当前 tab
    const bool f0 = (idx == 0), f1 = (idx == 1), f2 = (idx == 2), f3 = (idx == 3);
    ui->actTabRegister->setChecked(f0);
    ui->actTabFault->setChecked(f1);
    ui->actTabScenario->setChecked(f2);
    ui->actTabLog->setChecked(f3);
}

void SimulatorMainWindow::closeEvent(QCloseEvent* e) {
    onStopEngine();   // 优雅停止：DataTick join + 场景报告落盘
    QMainWindow::closeEvent(e);
}
