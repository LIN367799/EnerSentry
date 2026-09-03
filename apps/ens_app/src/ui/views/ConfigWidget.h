// src/ui/views/ConfigWidget.h —— L5 参数配置（ENS-LLD-504，切片 23）。
// 点表只读浏览 + 链路参数显示。
// 切片 41（FR-EXP-05/06）：新增"导出/备份"页——导出点表+规则配置 JSON、
// 备份历史/告警 SQLite 月库（ExportUtils 纯文件拷贝）。
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
    /// @param pt          点表（共享只读）
    /// @param rulesPath   告警规则 JSON 路径（可为空）
    /// @param ptPath      点表源 JSON 路径（导出配置用；可为空）
    /// @param dataRoot    历史/告警月库根目录（--data-dir；空 → 备份按钮禁用）
    ConfigWidget(const std::shared_ptr<ens::protocol::PointTable>& pt, const QString& rulesPath,
                 int ruleCount, const QString& host, quint16 port, int pollMs,
                 const QString& ptPath = {}, const QString& dataRoot = {},
                 QWidget* parent = nullptr);
    ~ConfigWidget() override;

private slots:
    void onExportCfgClicked();    // FR-EXP-06：点表 + 规则 JSON 拷贝到选目录
    void onBackupClicked();       // FR-EXP-05：history/ 与 alarm/ 月库递归拷贝

private:
    void fillPointTable(const std::shared_ptr<ens::protocol::PointTable>& pt);

    Ui::ConfigWidget* ui;
    QStandardItemModel* m_ptModel;
    QString m_rulesPath;
    QString m_ptPath;
    QString m_dataRoot;
};

}  // namespace ens::ui
