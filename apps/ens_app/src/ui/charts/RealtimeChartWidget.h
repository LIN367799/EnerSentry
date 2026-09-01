// src/ui/charts/RealtimeChartWidget.h —— L5 实时曲线容器（ENS-LLD-503，切片 20）。
// 持 RealtimePlotWidget + DataBus 订阅桥：wildcard 订阅 → 按 pointId 自动建通道（≤8），
// 转发样本（QueuedConnection 跨线程安全投递，绝不在 onSample 触碰 UI）。
#pragma once

#include <QWidget>

#include <cstdint>

#include "DataBus.h"

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

private:
    ens::datahub::DataBus* m_bus;
    RealtimePlotWidget* m_plot;
    RealtimeChartSubscriber m_sub;
    ens::datahub::Subscription m_handle = 0;
};

}  // namespace ens::ui
