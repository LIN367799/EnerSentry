// src/ui/views/HistoryTrendWidget.h —— L5 历史趋势（ENS-LLD-505，切片 24）。
// 测点/粒度/时间范围选择 → IDataAccess::queryRange（抽象注入，严禁触碰 SQLiteDataAccess）→
// QCustomPlot 静态曲线（avgValue 时间序列）。
// 切片 34（FR-EXP-01）：查询成功后缓存结果上下文，"导出 CSV"经 CsvWriter 落盘
// （列：时间戳/测点ID/测点名称/数值/单位，UTF-8 BOM，Excel 兼容）。
#pragma once

#include "DownSampler.h"   // DownSampledSample 完整类型（vector 成员）

#include <QWidget>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ens::datahub {
class IDataAccess;
}  // namespace ens::datahub

namespace ens::protocol {
class PointTable;
struct PointRuntime;
}  // namespace ens::protocol

namespace Ui {
class HistoryTrendWidget;
}

namespace ens::ui {

class HistoryTrendWidget : public QWidget {
    Q_OBJECT
public:
    HistoryTrendWidget(ens::datahub::IDataAccess* dal,
                       const std::shared_ptr<ens::protocol::PointTable>& pt,
                       QWidget* parent = nullptr);
    ~HistoryTrendWidget() override;

private slots:
    void onQueryClicked();
    void onExportClicked();

private:
    void fillPoints(const std::shared_ptr<ens::protocol::PointTable>& pt);
    /// 点表查询测点（导出"单位"列；点已删/无表时返回空）
    std::string unitOf(uint32_t pointId) const;

    Ui::HistoryTrendWidget* ui;
    ens::datahub::IDataAccess* m_dal;
    std::shared_ptr<ens::protocol::PointTable> m_pt;
    std::vector<uint32_t> m_pointIds;   // combo index → pointId

    // —— 最近一次成功查询的导出上下文（FR-EXP-01）——
    bool m_hasResult = false;
    std::vector<ens::datahub::DownSampledSample> m_lastRows;
    uint32_t m_lastPointId = 0;
    std::string m_lastPointName;   // 显示名（combo 文本首段）
    std::string m_lastGranText;    // 粒度显示文本（文件名/表头备查）
};

}  // namespace ens::ui
