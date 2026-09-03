// src/ui/views/overview_widget.h —— L5 电站总览（ENS-LLD-502；切片 19/40）。
// 切片 40（FR-OV-06/07）：SOC 仪表盘（整站 = Rack SOC 算术平均）+ 簇最高温热力条；
// 订阅 DataBus wildcard：onSample 仅原子计数/缓存（非阻塞），500ms QTimer 刷 UI。
// 簇归属与量测类型按点表 pointName 约定解析（ui/common/OverviewPoints.h）。
#pragma once

#include <QAtomicInt>
#include <QLabel>
#include <QMutex>
#include <QTimer>
#include <QVariant>
#include <QVector>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <memory>

#include "DataBus.h"

class QVBoxLayout;

namespace ens::protocol {
class PointTable;
}  // namespace ens::protocol

namespace Ui {
class OverviewWidget;
}

namespace ens::ui {

class SocGauge;
class TempHeatBar;

/// DataBus 订阅桥（非阻塞：onSample 仅原子写 + 轻量缓存，绝不触碰 UI）
class OverviewSubscriber final : public ens::datahub::IDataBusSubscriber {
public:
    void onSample(const ens::datahub::Sample& s) noexcept override {
        m_count.fetch_add(1, std::memory_order_relaxed);
        m_lastPid.store(s.pointId, std::memory_order_relaxed);
        m_lastVal.store(s.value, std::memory_order_relaxed);
        QMutexLocker lock(&m_cacheLock);
        m_cache.insert(s.pointId, s.value);
    }
    uint64_t count() const { return m_count.load(std::memory_order_relaxed); }
    uint32_t lastPid() const { return m_lastPid.load(std::memory_order_relaxed); }
    float    lastVal() const { return m_lastVal.load(std::memory_order_relaxed); }
    /// 最近样本缓存（UI 500ms 读取；缺返回 false）
    bool cached(uint32_t pointId, float* out) const {
        QMutexLocker lock(&m_cacheLock);
        auto it = m_cache.constFind(pointId);
        if (it == m_cache.constEnd()) return false;
        *out = it.value();
        return true;
    }
private:
    std::atomic<uint64_t> m_count{0};
    std::atomic<uint32_t> m_lastPid{0};
    std::atomic<float>    m_lastVal{0.0f};
    mutable QMutex        m_cacheLock;
    QHash<uint32_t, float> m_cache;
};

class OverviewWidget : public QWidget {
    Q_OBJECT
public:
    /// @param pt 点表（簇归属/SOC 分类源；null → 仪表与热力条禁用，兼容 ui_smoke）
    explicit OverviewWidget(ens::datahub::DataBus* bus,
                            const std::shared_ptr<ens::protocol::PointTable>& pt = {},
                            QWidget* parent = nullptr);
    ~OverviewWidget() override;

    // ── 测试观测（500ms 刷新后值）──
    double lastSoc() const noexcept { return m_lastSoc; }
    int    heatCellCount() const noexcept { return m_heatCount; }
    /// 立即执行一次 UI 刷新（生产由 500ms timer 驱动；测试显式触发用）
    void refreshNow() { onRefreshUi(); }

private slots:
    void onRefreshUi();

private:
    void buildPointIndex();   // 点表 → SOC 点 / 簇温度点索引

    Ui::OverviewWidget* ui;
    ens::datahub::DataBus* m_bus;
    OverviewSubscriber m_sub;
    ens::datahub::Subscription m_handle = 0;
    QTimer m_timer;   // 500ms UI 刷新
    std::shared_ptr<ens::protocol::PointTable> m_pt;

    SocGauge*    m_gauge = nullptr;
    TempHeatBar* m_heat  = nullptr;

    QVector<uint32_t> m_socIds;               // Rack SOC 点
    QVector<int>      m_rackOrder;            // 簇号升序
    QHash<int, uint32_t> m_maxTempByRack;     // rackNo → MaxTemp pointId
    double m_lastSoc = 0.0;
    int    m_heatCount = 0;
};

}  // namespace ens::ui
