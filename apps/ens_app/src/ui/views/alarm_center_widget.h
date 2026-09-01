// src/ui/views/alarm_center_widget.h —— L5 告警中心（ENS-LLD-504 骨架，切片 19）。
// 连接 AlarmEngine::alarmTriggered（QueuedConnection，AlarmEvent 已 qRegisterMetaType）
// 追加到表格；1s QTimer 轮询 activeAlarmCount 刷新计数。
// 完整 AlarmTableViewModel（风暴保护/确认工具条/导出）属后续切片。
#pragma once

#include <QStandardItemModel>
#include <QTimer>
#include <QWidget>

#include "AlarmEntities.h"   // AlarmEvent 完整类型（信号参数）

namespace ens::business {
class AlarmEngine;
}  // namespace ens::business

namespace Ui {
class AlarmCenterWidget;
}

namespace ens::ui {

class AlarmCenterWidget : public QWidget {
    Q_OBJECT
public:
    explicit AlarmCenterWidget(ens::business::AlarmEngine* alarm, QWidget* parent = nullptr);
    ~AlarmCenterWidget() override;

private slots:
    void onAlarmTriggered(const ens::business::AlarmEvent& ev);
    void onRefreshActive();

private:
    void appendRow(const ens::business::AlarmEvent& ev);

    Ui::AlarmCenterWidget* ui;
    ens::business::AlarmEngine* m_alarm;
    QStandardItemModel* m_model;
    QTimer m_timer;   // 1s 活跃计数刷新
};

}  // namespace ens::ui
