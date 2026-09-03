// src/ui/controls/SocGauge.h —— SOC 仪表盘（切片 40，FR-OV-06）。
// QPainter 自绘 240° 圆弧仪表：背景灰环 + 三段语义色带（红 <20 / 黄 20-50 / 绿 50-100）
// + 当前值高亮弧 + 中心大字百分比。无需 .ui / 无外部依赖，暗色主题内建。
#pragma once

#include <QWidget>

namespace ens::ui {

class SocGauge : public QWidget {
    Q_OBJECT
public:
    explicit SocGauge(QWidget* parent = nullptr);

    /// 设置 SOC 值（0-100；越界钳制）
    void setValue(double soc);
    double value() const noexcept { return m_value; }
    void setTitle(const QString& t);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    double  m_value = 0.0;
    QString m_title = QStringLiteral("整站 SOC");
};

}  // namespace ens::ui
