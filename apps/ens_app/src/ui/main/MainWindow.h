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
#include <QPointer>
#include <QTimer>

#include <functional>
#include <memory>

#include "AlarmEntities.h"   // 切片 37：onCriticalAlarm slot 参数（AlarmEvent 完整类型）

namespace ens::channel {
class IChannel;
}  // namespace ens::channel

namespace ens::protocol {
class PointTable;
}  // namespace ens::protocol

namespace ens::business {
class AlarmEngine;
class AuthManager;
class SboStateMachine;
struct SboSelectRequest;
}  // namespace ens::business

namespace ens::datahub {
class DataBus;
class IDataAccess;
class IAlarmAccess;
}  // namespace ens::datahub

namespace Ui {
class MainWindow;
}

namespace ens::ui {

class OverviewWidget;
class AlarmCenterWidget;
class RealtimeChartWidget;
class SBOControlWidget;
class DiagWidget;
class ConfigWidget;
class HistoryTrendWidget;
class AlarmNotifier;             // 切片 37：严重告警通知决策
class CriticalAlarmDialog;       // 切片 37：非模态告警弹窗

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

    // 切片 23：Diag/Config 数据源
    ens::channel::IChannel* channel = nullptr;
    std::shared_ptr<protocol::PointTable> pointTable;
    QString alarmRulesPath;
    int     alarmRuleCount = 0;
    QString host;
    quint16 port = 0;
    int     pollMs = 0;
    // 切片 24：历史查询（HistoryTrendWidget）
    ens::datahub::IDataAccess* dataAccess = nullptr;
    // 切片 36：告警历史查询（AlarmCenterWidget，FR-AL-11；未启用 --data-dir 时查询返空）
    ens::datahub::IAlarmAccess* alarmAccess = nullptr;
    // 切片 31：用户表路径（UserManagerDialog 保存目标）
    QString usersPath;

    QString linkLabel;      // 状态栏：连接标识
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const UiDeps& deps, QWidget* parent = nullptr);
    ~MainWindow() override;

    /// RBAC 裁剪入口（切片 26 落地：Operator 禁 SBO/Config；Engineer/Admin 全量）
    void applyPermissionFilter();

public slots:
    /// 链路连接状态（切片 22，5C 联调）：main.cpp connect EnerSentryApp::connected/disconnected
    void setLinkConnected(bool connected);

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;   // 切片 26：全局活动检测（FR-AUTH-05）

private slots:
    void switchView(int index);
    void onLockClicked();
    void onAuditClicked();   // 切片 30：审计日志对话框
    void onUsersClicked();   // 切片 31：用户管理对话框
    void onAbout();
    void onStatusTick();   // 1s：时钟 + idle 超时检查 + 告警计数
    void onCriticalAlarm(const ens::business::AlarmEvent& ev);   // 切片 37：FR-AL-06 声光

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
    DiagWidget*         m_diagView  = nullptr;
    ConfigWidget*       m_configView = nullptr;
    HistoryTrendWidget* m_historyView = nullptr;

    QLabel* m_lblLink  = nullptr;
    QLabel* m_lblClock = nullptr;
    QLabel* m_lblAlarm = nullptr;
    QLabel* m_lblUser  = nullptr;
    QTimer  m_statusTimer;
    bool    m_locked = false;

    // 切片 37：严重告警声光（AlarmNotifier 决策 → 弹窗/蜂鸣/任务栏闪烁）
    AlarmNotifier* m_notifier = nullptr;                    // 子对象（parent this）
    QPointer<CriticalAlarmDialog> m_criticalDlg;            // 非模态单例（关闭自动置空）
};

}  // namespace ens::ui
