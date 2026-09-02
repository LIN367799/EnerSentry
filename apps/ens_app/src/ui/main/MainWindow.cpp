// src/ui/main/MainWindow.cpp —— 主窗口框架实现（切片 19；切片 21：UiDeps + SBO + QSettings）。
#include "main/MainWindow.h"
#include "ui_MainWindow.h"

#include "auth/SessionLockDialog.h"
#include "charts/RealtimeChartWidget.h"
#include "common/AlarmNotifier.h"
#include "controls/AuditLogDialog.h"
#include "controls/CriticalAlarmDialog.h"
#include "controls/SBOControlWidget.h"
#include "controls/UserManagerDialog.h"
#include "views/ConfigWidget.h"
#include "views/DiagWidget.h"
#include "views/HistoryTrendWidget.h"
#include "views/alarm_center_widget.h"
#include "views/overview_widget.h"
#include "views/placeholder_view.h"

#include "AlarmEngine.h"
#include "AuthManager.h"
#include "PointTable.h"   // UiDeps::pointTable shared_ptr 析构需完整类型

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QEvent>
#include <QGuiApplication>
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
    connect(ui->actAudit, &QAction::triggered, this, &MainWindow::onAuditClicked);
    connect(ui->actUsers, &QAction::triggered, this, &MainWindow::onUsersClicked);
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
    applyPermissionFilter();   // 切片 26：RBAC 视图裁剪（restore 可能切到越权视图，此处置底）
    qApp->installEventFilter(this);   // 切片 26：全局活动检测 → touchActivity（FR-AUTH-05）
    updateIdentityLabel();

    // ── 切片 37：严重告警声光（FR-AL-06）──
    // AlarmNotifier 做通知决策（Critical/1s 防抖/风暴抑制），本窗消费信号弹窗+蜂鸣+任务栏闪烁
    if (m_deps.alarm) {
        m_notifier = new AlarmNotifier(m_deps.alarm, this);
        connect(m_notifier, &AlarmNotifier::criticalAlarm,
                this, &MainWindow::onCriticalAlarm);
    }
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
    ui->actAudit->setIcon(QIcon(QStringLiteral(":/icons/view_history.svg")));
    ui->actUsers->setIcon(QIcon(QStringLiteral(":/icons/menu_about.svg")));
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
    // 切片 36：告警中心完整版（历史筛选/确认/导出；currentUser 供确认人记录，auth 空则禁用）
    // 切片 38：±30s 回放按钮（L1 快照读取注入，FR-AL-12）
    m_alarmView = new AlarmCenterWidget(
        m_deps.alarm, m_deps.alarmAccess, m_deps.pointTable,
        [this]() { return m_deps.auth ? m_deps.auth->currentUser() : QString(); },
        m_deps.l1Replay, this);
    m_chart     = new RealtimeChartWidget(m_deps.bus, this);
    m_sboView   = new SBOControlWidget(m_deps.sbo, m_deps.sboSelect, m_deps.sboOperate,
                                       m_deps.sboCancel, this);
    m_diagView  = new DiagWidget(m_deps.channel, this);
    m_configView = new ConfigWidget(m_deps.pointTable, m_deps.alarmRulesPath,
                                    m_deps.alarmRuleCount, m_deps.host, m_deps.port,
                                    m_deps.pollMs, this);
    m_historyView = new HistoryTrendWidget(m_deps.dataAccess, m_deps.pointTable, this);
    ui->centralStack->addWidget(m_overview);    // 0 总览
    ui->centralStack->addWidget(m_chart);       // 1 实时曲线（切片 20）
    ui->centralStack->addWidget(m_alarmView);   // 2 告警中心
    ui->centralStack->addWidget(m_historyView); // 3 历史趋势（切片 24）
    ui->centralStack->addWidget(m_configView);  // 4 参数配置（切片 23）
    ui->centralStack->addWidget(m_diagView);    // 5 通信诊断（切片 23）
    ui->centralStack->addWidget(m_sboView);     // 6 SBO 控制（切片 21）
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

void MainWindow::onAuditClicked() {
    if (!m_deps.auth) return;
    AuditLogDialog dlg(m_deps.auth, this);
    dlg.exec();
}

void MainWindow::onUsersClicked() {
    if (!m_deps.auth) return;
    UserManagerDialog dlg(m_deps.auth, m_deps.usersPath, this);
    dlg.exec();
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
    if (!m_deps.auth) return;
    // Operator：只读（禁 SBO 控制 / 参数配置）；Engineer/Admin 全量
    const bool isOperator = (m_deps.auth->currentRole() == ens::business::UserRole::Operator);
    ui->actViewSbo->setEnabled(!isOperator);
    ui->actViewConfig->setEnabled(!isOperator);
    // 审计日志 Admin-only（切片 30；FR-AUTH-04 审计不可被普通角色篡改）
    const bool isAdmin = (m_deps.auth->currentRole() == ens::business::UserRole::Admin);
    ui->actAudit->setEnabled(isAdmin);
    ui->actUsers->setEnabled(isAdmin);   // 用户管理 Admin-only（切片 31）
    // 越权视图兜底：恢复/登录时若停在禁页 → 回总览
    const int cur = ui->centralStack->currentIndex();
    if (isOperator && (cur == 4 || cur == 6)) {
        switchView(0);
    }
}

bool MainWindow::eventFilter(QObject* obj, QEvent* e) {
    // 全局活动检测：鼠标/键盘/滚轮任意交互刷新空闲计时（FR-AUTH-05 真正生效）
    if (m_deps.auth && m_deps.auth->isLoggedIn()) {
        switch (e->type()) {
            case QEvent::MouseButtonPress:
            case QEvent::KeyPress:
            case QEvent::Wheel:
                m_deps.auth->touchActivity();
                break;
            default:
                break;
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::setLinkConnected(bool connected) {
    // 链路状态：已连接绿色 / 已断开红色（语义色，UI-02 系）
    if (connected) {
        m_lblLink->setText(QStringLiteral("● 已连接 %1").arg(m_deps.linkLabel));
        m_lblLink->setStyleSheet(QStringLiteral("color: #66bb6a;"));
    } else {
        m_lblLink->setText(QStringLiteral("● 已断开 %1").arg(m_deps.linkLabel));
        m_lblLink->setStyleSheet(QStringLiteral("color: #e94560;"));
    }
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

// ── 切片 37：FR-AL-06 严重告警声光 ──
// AlarmNotifier 已决策（Critical + 防抖 + 非风暴）；本槽执行展示：弹窗 + 蜂鸣 + 任务栏闪烁。
void MainWindow::onCriticalAlarm(const ens::business::AlarmEvent& ev) {
    QApplication::beep();               // 蜂鸣音
    QApplication::alert(this);          // 任务栏闪烁（窗口非活动时；活动窗无任务栏态）

    // 非模态红色弹窗：单例复用（防抖窗口内多条 Critical 刷新同实例，不堆叠）
    QString pointName;
    if (m_deps.pointTable) {
        const auto* p = m_deps.pointTable->pointIdOf(ev.pointId);
        if (p) pointName = QString::fromStdString(p->pointName);
    }
    if (m_criticalDlg.isNull()) {
        m_criticalDlg = new CriticalAlarmDialog(ev, pointName, this);
        m_criticalDlg->show();
    } else {
        m_criticalDlg->setEvent(ev, pointName);
        m_criticalDlg->raise();
        m_criticalDlg->activateWindow();
    }
}

}  // namespace ens::ui
