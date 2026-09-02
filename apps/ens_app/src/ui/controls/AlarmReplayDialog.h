// src/ui/controls/AlarmReplayDialog.h —— 告警 ±30s 高频回放（切片 38，FR-AL-12）。
// 展示层纯组件：调用方（AlarmCenterWidget）经 IL1SnapshotReader 拉取 L1 内存样本后
// setData() 喂入，本对话框负责渲染 QCustomPlot 曲线 + 空数据提示。
// 数据源边界：仅 L1 内存（~1h 滚动窗口）；淘汰后样本数为 0 → 明确提示。
#pragma once

#include <QDialog>

#include <cstdint>
#include <vector>

namespace ens::datahub {
struct Sample;
}  // namespace ens::datahub

namespace Ui {
class AlarmReplayDialog;
}

namespace ens::ui {

class AlarmReplayDialog : public QDialog {
    Q_OBJECT
public:
    explicit AlarmReplayDialog(QWidget* parent = nullptr);
    ~AlarmReplayDialog() override;

    /// 喂入回放数据并渲染。
    /// @param triggerTime 告警触发时刻（Unix ms）；x 轴 = (ts - triggerTime)/1000 秒（±30）
    void setData(uint32_t pointId, const QString& pointName, uint64_t triggerTime,
                 const std::vector<ens::datahub::Sample>& samples);

private:
    Ui::AlarmReplayDialog* ui;
};

}  // namespace ens::ui
