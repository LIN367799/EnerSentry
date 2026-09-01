// src/ui/views/ConfigWidget.h —— L5 参数配置（ENS-LLD-506 骨架，切片 23）。
// 三页：点表（allPoints 列表 + 统计）/ 告警规则（文件 + 条数，完整编辑属后续）/ 链路配置（只读）。
// 数据源经 UiDeps 注入（PointTable 共享只读 + 规则路径/条数 + host/port/pollMs）。
#pragma once

#include <QStandardItemModel>
#include <QWidget>

#include <memory>

namespace ens::protocol {
class PointTable;
}  // namespace ens::protocol

namespace Ui {
class ConfigWidget;
}

namespace ens::ui {

class ConfigWidget : public QWidget {
    Q_OBJECT
public:
    ConfigWidget(const std::shared_ptr<protocol::PointTable>& pt, const QString& rulesPath,
                 int ruleCount, const QString& host, quint16 port, int pollMs,
                 QWidget* parent = nullptr);
    ~ConfigWidget() override;

private:
    void fillPointTable(const std::shared_ptr<protocol::PointTable>& pt);

    Ui::ConfigWidget* ui;
    QStandardItemModel* m_ptModel;
};

}  // namespace ens::ui
