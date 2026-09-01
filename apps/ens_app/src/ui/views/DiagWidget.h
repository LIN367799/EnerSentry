// src/ui/views/DiagWidget.h —— L5 通信诊断（ENS-LLD-507，切片 23）。
// 链路质量表（IChannel::getStats 无锁快照：requestTotal/timeoutCount/crcErrorCount/successRate）
// + 请求吞吐曲线（1s 轮询差值 → 复用 RealtimePlotWidget 单通道）。
// 依赖注入 channel::IChannel*（ens::ui 经依赖链传递可达，不触碰 app 层）。
#pragma once

#include <QLabel>
#include <QTimer>
#include <QWidget>

#include <cstdint>

namespace ens::channel {
class IChannel;
}  // namespace ens::channel

namespace Ui {
class DiagWidget;
}

namespace ens::ui {

class RealtimePlotWidget;

class DiagWidget : public QWidget {
    Q_OBJECT
public:
    explicit DiagWidget(ens::channel::IChannel* ch, QWidget* parent = nullptr);
    ~DiagWidget() override;

private slots:
    void onRefreshTick();   // 1s：统计表 + 吞吐曲线

private:
    Ui::DiagWidget* ui;
    ens::channel::IChannel* m_ch;
    RealtimePlotWidget* m_plot;
    QTimer m_timer;
    uint64_t m_lastTotal = 0;
    int m_tick = 0;
};

}  // namespace ens::ui
