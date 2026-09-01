// src/ui/main/MainWindow.cpp —— 主窗口框架实现（切片 19；切片 21：UiDeps + SBO + QSettings）。
#include "main/MainWindow.h"
#include "ui_MainWindow.h"

#include "auth/SessionLockDialog.h"
#include "charts/RealtimeChartWidget.h"
#include "controls/SBOControlWidget.h"
#include "views/alarm_center_widget.h"
#include "views/overview_widget.h"
#include "views/placeholder_view.h"

#include "AlarmEngine.h"
#include "AuthManager.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QIcon>
#include <QMessageBox>
#include <QSettings>
#include <QVector>

namespace ens::ui {

namespace {
QString roleText(ens::business::UserRole r) {
    switch (r) {
        case ens::business::UserRole::Admin:    return QStringLiteral("管理员");
        case ens::business::UserRole::Engineer: return QStringLiteral("工程师");
        case ens::business::UserRole::Operator: return QStringLiteral("操作员");
    }
    return QStringLiteral("?");
}
}  // namespace

MainWindow::MainWindow(const UiDeps& deps, QWidget* parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_deps(deps) {
    ui->setupUi(this);
    setupIcons();
    setupViews();

    // ── 视图切换（菜单/工具栏动作 → CentralStack）──
    const QVector<QAction*> acts = {
        ui->actViewOverview, ui->actViewTrend, ui->actViewAlarm, ui->actViewHistory,
        ui->actViewConfig,   ui->actViewDiag,  ui->actViewSbo};
    for (int i = 0; i < acts.size(); ++i) {
        connect(acts[i], &QAction::triggered, this, [this, i] { switchView(i); });
    }

    connect(ui->actLock,  &QAction::triggered, this, &MainWindow::onLockClicked);
    connect(ui->actExit,  &QAction::triggered, this, &MainWindow::close);
    connect(ui->actAbout, &QAction::triggered, this, &MainWindow::onAbout);

    // ── 状态栏 ──
    m_lblLink  = new QLabel(m_deps.linkLabel, this);
    m_lblClock = new QLabel(QStringLiteral("--:--:--"), this);
    m_lblAlarm = new QLabel(QStringLiteral("告警 0"), this);
    m_lblUser  = new QLabel(QStringLiteral("未登录"), this);
    statusBar()->addWidget(m_lblLink);
    statusBar()->addPermanentWidget(m_lblClock);
    statusBar()->addPermanentWidget(m_lblAlarm);
    statusBar()->addPermanentWidget(m_lblUser);

    m_statusTimer.setInterval(1000);
    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::onStatusTick);
    m_statusTimer.start();

    restoreWindowState();   // QSettings：几何 + 最后视图（切片 21）
    updateIdentityLabel();
}

MainWindow::~MainWindow() {
    m_statusTimer.stop();
    delete ui;
}

void MainWindow::setupIcons() {
    setWindowIcon(QIcon(QStringLiteral(":/icons/app_logo.svg")));
    ui->actViewOverview->setIcon(QIcon(QStringLiteral(":/icons/view_overview.svg")));
    ui->actViewTrend->setIcon(QIcon(QStringLiteral(":/icons/view_trend.svg")));
    ui->actViewAlarm->setIcon(QIcon(QStringLiteral(":/icons/view_alarm.svg")));
    ui->actViewHistory->setIcon(QIcon(QStringLiteral(":/icons/view_history.svg")));
    ui->actViewConfig->setIcon(QIcon(QStringLiteral(":/icons/view_config.svg")));
    ui->actViewDiag->setIcon(QIcon(QStringLiteral(":/icons/view_diag.svg")));
    ui->actViewSbo->setIcon(QIcon(QStringLiteral(":/icons/view_sbo.svg")));
    ui->actLock->setIcon(QIcon(QStringLiteral(":/icons/lock.svg")));
    ui->actExit->setIcon(QIcon(QStringLiteral(":/icons/menu_quit.svg")));
    ui->actAbout->setIcon(QIcon(QStringLiteral(":/icons/menu_about.svg")));
}

void MainWindow::setupViews() {
    // 移除 .ui 中的 7 个占位空页，按固定顺序 addWidget 真实视图（index 0..6）
    while (ui->centralStack->count() > 0) {
        QWidget* p = ui->centralStack->widget(0);
        ui->centralStack->removeWidget(p);
        p->deleteLater();
    }

    m_overview  = new OverviewWidget(m_deps.bus, this);
    m_alarmView = new AlarmCenterWidget(m_deps.alarm, this);
    m_chart     = new RealtimeChartWidget(m_deps.bus, this);
    m_sboView   = new SBOControlWidget(m_deps.sbo, m_deps.sboSelect, m_deps.sboOperate,
                                       m_deps.sboCancel, this);
    ui->centralStack->addWidget(m_overview);   // 0 总览
    ui->centralStack->addWidget(m_chart);      // 1 实时曲线（切片 20）
    ui->centralStack->addWidget(m_alarmView);  // 2 告警中心
    // 3 历史趋势
    ui->centralStack->addWidget(new PlaceholderView(
        QStringLiteral("历史趋势"), QStringLiteral("ENS-LLD-505 HistoryTrendWidget（IDataAccess 历史查询）属后续切片。"), this));
    // 4 参数配置
    ui->centralStack->addWidget(new PlaceholderView(
        QStringLiteral("参数配置"), QStringLiteral("ENS-LLD-506 ConfigWidget（点表/阈值/链路分页）属后续切片。"), this));
    // 5 通信诊断
    ui->centralStack->addWidget(new PlaceholderView(
        QStringLiteral("通信诊断"), QStringLiteral("ENS-LLD-507 DiagWidget（IChannel::getStats 链路质量）属后续切片。"), this));
    ui->centralStack->addWidget(m_sboView);    // 6 SBO 控制（切片 21）
}

void MainWindow::switchView(int index) {
    ui->centralStack->setCurrentIndex(index);
    const QVector<QAction*> acts = {
        ui->actViewOverview, ui->actViewTrend, ui->actViewAlarm, ui->actViewHistory,
        ui->actViewConfig,   ui->actViewDiag,  ui->actViewSbo};
    for (int i = 0; i < acts.size(); ++i) acts[i]->setChecked(i == index);
}

void MainWindow::onLockClicked() {
    doLock();
}

void MainWindow::doLock() {
    if (!m_deps.auth || !m_deps.auth->isLoggedIn() || m_locked) return;
    m_deps.auth->lock();
    m_locked = true;
    SessionLockDialog dlg(m_deps.auth, this);
    const int rc = dlg.exec();
    m_locked = false;
    if (rc == QDialog::Accepted) {
        updateIdentityLabel();
    } else {
        // 解锁失败/关闭：退出登录，回到登录首屏（close 由 main 决定）
        m_deps.auth->logout();
        close();
    }
}

void MainWindow::onAbout() {
    QMessageBox::about(this, QStringLiteral("关于 EnerSentry"),
        QStringLiteral(
            "<b>EnerSentry 储能上位机</b><br>"
            "Phase 4 切片 21：SBO 控制面板 + OpenGL/High DPI/QSettings<br><br>"
            "五层架构：channel → protocol → datahub → business → ui<br>"
            "图标：Feather Icons v4.29.2 (MIT)"));
}

void MainWindow::onStatusTick() {
    // 时钟
    m_lblClock->setText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));
    // 告警计数
    if (m_deps.alarm) {
        m_lblAlarm->setText(QStringLiteral("告警 %1").arg(m_deps.alarm->activeAlarmCount()));
    }
    // 会话超时自动锁定（FR-AUTH-05）
    if (m_deps.auth && m_deps.auth->isLoggedIn() && !m_locked &&
        m_deps.auth->idleSeconds() >= kIdleLockSeconds) {
        doLock();
    }
}

void MainWindow::updateIdentityLabel() {
    if (m_deps.auth && m_deps.auth->isLoggedIn()) {
        m_lblUser->setText(QStringLiteral("%1 · %2")
                               .arg(m_deps.auth->currentUser(), roleText(m_deps.auth->currentRole())));
    } else {
        m_lblUser->setText(QStringLiteral("未登录"));
    }
}

void MainWindow::applyPermissionFilter() {
    // V1.6：按 AuthManager::currentRole() 裁剪 menuFile/menuTools 项
    // （Admin 全量 / Engineer 控制+配置 / Operator 只读）。当前空实现占位。
}

void MainWindow::restoreWindowState() {
    QSettings s(QStringLiteral("EnerSentry"), QStringLiteral("ens_app"));
    const QByteArray geo = s.value(QStringLiteral("main/geometry")).toByteArray();
    if (!geo.isEmpty()) restoreGeometry(geo);
    const int view = s.value(QStringLiteral("main/viewIndex"), 0).toInt();
    if (view >= 0 && view < ui->centralStack->count()) {
        switchView(view);
    }
}

void MainWindow::saveWindowState() {
    QSettings s(QStringLiteral("EnerSentry"), QStringLiteral("ens_app"));
    s.setValue(QStringLiteral("main/geometry"), saveGeometry());
    s.setValue(QStringLiteral("main/viewIndex"), ui->centralStack->currentIndex());
}

void MainWindow::closeEvent(QCloseEvent* e) {
    saveWindowState();   // 切片 21：窗口几何 + 最后视图持久化
    QMainWindow::closeEvent(e);
}

}  // namespace ens::ui
