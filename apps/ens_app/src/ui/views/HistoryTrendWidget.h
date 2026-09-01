// src/ui/views/HistoryTrendWidget.h —— L5 历史趋势（ENS-LLD-505，切片 24）。
// 测点/粒度/时间范围选择 → IDataAccess::queryRange（抽象注入，严禁触碰 SQLiteDataAccess）→
// QCustomPlot 静态曲线（avgValue 时间序列）。
#pragma once

#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

namespace ens::datahub {
class IDataAccess;
}  // namespace ens::datahub

namespace ens::protocol {
class PointTable;
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

private:
    void fillPoints(const std::shared_ptr<ens::protocol::PointTable>& pt);

    Ui::HistoryTrendWidget* ui;
    ens::datahub::IDataAccess* m_dal;
    std::vector<uint32_t> m_pointIds;   // combo index → pointId
};

}  // namespace ens::ui
