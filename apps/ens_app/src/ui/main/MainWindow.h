// src/ui/main/MainWindow.h —— L5 主窗口框架（ENS-LLD-501，切片 19）。
// 依赖注入（LLD-500 §0.4 铁律：ens::ui 绝不 include app 层）：
//   * datahub::DataBus*          —— 实时数据订阅（Overview 等）
//   * business::AlarmEngine*     —— 告警信号 + activeAlarmCount
//   * business::AuthManager*     —— 会话/锁定/身份（FR-AUTH-01/05）
// CentralStack 7 视图：Overview/AlarmCenter 接真实数据，其余占位（后续切片逐个实装）。
// RBAC 菜单裁剪：applyPermissionFilter() 接口占位（V1.6 完整引擎接入）。
// 会话超时锁屏：1s QTimer 轮询 auth->idleSeconds() >= kIdleLockSeconds(900) → lock。
#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QTimer>

namespace ens::business {
class AlarmEngine;
class AuthManager;
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

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(ens::datahub::DataBus* bus, ens::business::AlarmEngine* alarm,
               ens::business::AuthManager* auth, const QString& linkLabel,
               QWidget* parent = nullptr);
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

    static constexpr int kIdleLockSeconds = 900;   // FR-AUTH-05：15 分钟无操作自动锁定

    Ui::MainWindow* ui;
    ens::datahub::DataBus*      m_bus;
    ens::business::AlarmEngine* m_alarm;
    ens::business::AuthManager* m_auth;
    QString m_linkLabel;

    OverviewWidget*     m_overview  = nullptr;
    AlarmCenterWidget*  m_alarmView = nullptr;
    RealtimeChartWidget* m_chart    = nullptr;

    QLabel* m_lblLink  = nullptr;
    QLabel* m_lblClock = nullptr;
    QLabel* m_lblAlarm = nullptr;
    QLabel* m_lblUser  = nullptr;
    QTimer  m_statusTimer;
    bool    m_locked = false;
};

}  // namespace ens::ui
