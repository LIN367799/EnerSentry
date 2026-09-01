// src/ui/views/overview_widget.h —— L5 电站总览（ENS-LLD-502 骨架，切片 19）。
// 订阅 DataBus wildcard：onSample 内仅原子计数/缓存（非阻塞），500ms QTimer 刷 UI。
// 三级钻取 DrillDownNavigator 属完整版（切片 20+），本版展示实时流量概览。
#pragma once

#include <QAtomicInt>
#include <QLabel>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <cstdint>

#include "DataBus.h"

namespace Ui {
class OverviewWidget;
}

namespace ens::ui {

/// DataBus 订阅桥（非阻塞：onSample 仅原子写，绝不触碰 UI）
class OverviewSubscriber final : public ens::datahub::IDataBusSubscriber {
public:
    void onSample(const ens::datahub::Sample& s) noexcept override {
        m_count.fetch_add(1, std::memory_order_relaxed);
        m_lastPid.store(s.pointId, std::memory_order_relaxed);
        m_lastVal.store(s.value, std::memory_order_relaxed);
    }
    uint64_t count() const { return m_count.load(std::memory_order_relaxed); }
    uint32_t lastPid() const { return m_lastPid.load(std::memory_order_relaxed); }
    float    lastVal() const { return m_lastVal.load(std::memory_order_relaxed); }
private:
    std::atomic<uint64_t> m_count{0};
    std::atomic<uint32_t> m_lastPid{0};
    std::atomic<float>    m_lastVal{0.0f};
};

class OverviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit OverviewWidget(ens::datahub::DataBus* bus, QWidget* parent = nullptr);
    ~OverviewWidget() override;

private slots:
    void onRefreshUi();

private:
    Ui::OverviewWidget* ui;
    ens::datahub::DataBus* m_bus;
    OverviewSubscriber m_sub;
    ens::datahub::Subscription m_handle = 0;
    QTimer m_timer;   // 500ms UI 刷新
};

}  // namespace ens::ui
