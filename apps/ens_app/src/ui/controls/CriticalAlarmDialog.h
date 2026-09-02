// src/ui/controls/CriticalAlarmDialog.h —— 严重告警弹窗（切片 37，FR-AL-06）。
// 非模态红色告警提示：展示触发时间/测点/告警值/阈值/描述；"我知道了"关闭。
// 生命周期由 MainWindow 以 QPointer 管理（多条 Critical 由 AlarmNotifier 1s 防抖限流，
// 本弹窗常驻直到用户关闭；重复触发时复用实例更新内容）。
#pragma once

#include <QDialog>

#include "AlarmEntities.h"   // AlarmEvent

namespace Ui {
class CriticalAlarmDialog;
}

namespace ens::ui {

class CriticalAlarmDialog : public QDialog {
    Q_OBJECT
public:
    /// @param pointName 测点显示名（可为空 → 仅显示 pointId）
    explicit CriticalAlarmDialog(const ens::business::AlarmEvent& ev,
                                 const QString& pointName,
                                 QWidget* parent = nullptr);
    ~CriticalAlarmDialog() override;

    /// 复用实例更新内容（防抖窗口内同实例刷新，避免弹窗堆积）
    void setEvent(const ens::business::AlarmEvent& ev, const QString& pointName);

private:
    Ui::CriticalAlarmDialog* ui;
};

}  // namespace ens::ui
