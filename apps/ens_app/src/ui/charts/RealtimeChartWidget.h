// src/ui/charts/RealtimeChartWidget.h —— L5 实时曲线容器（ENS-LLD-503，切片 20）。
// 持 RealtimePlotWidget + DataBus 订阅桥：wildcard 订阅 → 按 pointId 自动建通道（≤8），
// 转发样本（QueuedConnection 跨线程安全投递，绝不在 onSample 触碰 UI）。
// 切片 39（FR-RT-06/07/08）：外壳加标尺开关 + 通道列表（checkbox 分配左/右轴），
// 悬停读数默认开启（RealtimePlotWidget 内实现）。
#pragma once

#include <QWidget>

#include <cstdint>

#include "DataBus.h"

class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace ens::ui {

class RealtimePlotWidget;
class RealtimeChartWidget;

/// DataBus 订阅桥（非阻塞：仅转发指针，UI 更新经 QueuedConnection 主线程）
class RealtimeChartSubscriber final : public ens::datahub::IDataBusSubscriber {
public:
    explicit RealtimeChartSubscriber(RealtimeChartWidget* w) : m_w(w) {}
    void onSample(const ens::datahub::Sample& s) noexcept override;

private:
    RealtimeChartWidget* m_w;
};

class RealtimeChartWidget : public QWidget {
    Q_OBJECT
public:
    static constexpr int kMaxChannels = 8;

    explicit RealtimeChartWidget(ens::datahub::DataBus* bus, QWidget* parent = nullptr);
    ~RealtimeChartWidget() override;

    /// 订阅桥入口（任意线程可调；内部 QueuedConnection 投递到主线程）
    void onSampleBridge(const ens::datahub::Sample& s);

    /// 标尺开关（测试观测）
    bool rulerToggled() const;
    /// 通道列表行数（测试观测：订阅流自动建行）
    int channelListCount() const;

private slots:
    void onChannelAdded(uint32_t pointId, const QString& name);   // plot → 列表行
    void onChannelItemChanged(QListWidgetItem* item);              // 勾选 → 右轴分配
    void onRulerToggled(bool checked);
    void onPngClicked();                                           // 切片 41：FR-EXP-02

private:
    void addListRow(uint32_t pointId, const QString& name);

    ens::datahub::DataBus* m_bus;
    RealtimePlotWidget* m_plot;
    RealtimeChartSubscriber m_sub;
    ens::datahub::Subscription m_handle = 0;
    QListWidget* m_chList = nullptr;
    QPushButton* m_btnRuler = nullptr;
};

}  // namespace ens::ui
