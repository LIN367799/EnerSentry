// src/ui/main/MainWindow.h —— L5 主窗口框架（ENS-LLD-501，切片 19；切片 21 扩展）。
// 依赖注入（LLD-500 §0.4 铁律：ens::ui 绝不 include app 层）：
//   * datahub::DataBus*          —— 实时数据订阅（Overview/图表）
//   * business::AlarmEngine*     —— 告警信号 + activeAlarmCount
//   * business::AuthManager*     —— 会话/锁定/身份（FR-AUTH-01/05）
//   * business::SboStateMachine* —— SBO 状态机（按钮态联动）
//   * SBO 下发回调（std::function）—— main.cpp lambda 绑定 EnerSentryApp::submitSboXxx
// 聚合为 UiDeps 一次注入。
// CentralStack 7 视图：Overview/AlarmCenter/RealtimeChart/SBO 接真实数据，其余占位。
// RBAC 菜单裁剪：applyPermissionFilter() 接口占位（V1.6）。
// 会话超时锁屏：1s QTimer 轮询 idleSeconds() >= 900 → lock。
// QSettings 持久化（切片 21）：窗口几何 + 最后视图（closeEvent 保存 / 构造恢复）。
#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QTimer>

#include <functional>

namespace ens::business {
class AlarmEngine;
class AuthManager;
class SboStateMachine;
struct SboSelectRequest;
}  // namespace ens::business

namespace ens::datahub {
class DataBus;
}  // namespace ens::datahub

namespace Ui {
class MainWindow;
}

namespace ens::ui {

class OverviewWidget;
class AlarmCenterWidget;
class RealtimeChartWidget;
class SBOControlWidget;

/// 主窗口依赖聚合（main.cpp 一次构造传入；ens::ui 不触碰 app 层）
struct UiDeps {
    ens::datahub::DataBus*      bus    = nullptr;
    ens::business::AlarmEngine* alarm  = nullptr;
    ens::business::AuthManager* auth   = nullptr;
    ens::business::SboStateMachine* sbo = nullptr;

    using SubmitSelectFn  = std::function<bool(const ens::business::SboSelectRequest&)>;
    using SubmitOperateFn = std::function<bool(const QString&)>;
    using SubmitCancelFn  = std::function<bool(const QString&)>;
    SubmitSelectFn  sboSelect;
    SubmitOperateFn sboOperate;
    SubmitCancelFn  sboCancel;

    QString linkLabel;      // 状态栏：连接标识
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const UiDeps& deps, QWidget* parent = nullptr);
    ~MainWindow() override;

    /// RBAC 裁剪入口（V1.6 接入 AuthManager::checkPermission；当前空实现占位）
    void applyPermissionFilter();

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void switchView(int index);
    void onLockClicked();
    void onAbout();
    void onStatusTick();   // 1s：时钟 + idle 超时检查 + 告警计数

private:
    void setupViews();
    void setupIcons();
    void doLock();
    void updateIdentityLabel();
    void restoreWindowState();
    void saveWindowState();

    static constexpr int kIdleLockSeconds = 900;   // FR-AUTH-05：15 分钟无操作自动锁定

    Ui::MainWindow* ui;
    UiDeps m_deps;

    OverviewWidget*     m_overview  = nullptr;
    AlarmCenterWidget*  m_alarmView = nullptr;
    RealtimeChartWidget* m_chart    = nullptr;
    SBOControlWidget*   m_sboView   = nullptr;

    QLabel* m_lblLink  = nullptr;
    QLabel* m_lblClock = nullptr;
    QLabel* m_lblAlarm = nullptr;
    QLabel* m_lblUser  = nullptr;
    QTimer  m_statusTimer;
    bool    m_locked = false;
};

}  // namespace ens::ui
