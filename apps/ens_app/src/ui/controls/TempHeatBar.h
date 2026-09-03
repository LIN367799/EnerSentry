// src/ui/controls/TempHeatBar.h —— 簇最高温热力条（切片 40，FR-OV-07）。
// 横向 N 格：每格 = 一簇的 MaxTemp，颜色按温度梯度（蓝绿 → 黄 → 红，语义阈值 40/50℃），
// 悬停格显示 "Rack-01  34.2 ℃"（本控件绘制 + hoverText 供上层/测试读取）。
// 纯 QPainter 自绘（点表簇数动态，避免 QCPColorMap 单行网格尺度开销）。
#pragma once

#include <QVector>
#include <QWidget>

namespace ens::ui {

class TempHeatBar : public QWidget {
    Q_OBJECT
public:
    struct Cell {
        QString label;   // Rack-01
        float   temp = 0.0f;
        bool    valid = false;   // 尚无数据
    };

    explicit TempHeatBar(QWidget* parent = nullptr);

    /// 设置簇温度格（按传入序绘制；空 = 清空显示）
    void setCells(const QVector<Cell>& cells);
    const QVector<Cell>& cells() const noexcept { return m_cells; }
    /// 最近悬停格文本（测试观测）
    QString hoverText() const noexcept { return m_hoverText; }
    /// 测试钩子：像素 x → 悬停
    void notifyMouseMoved(int xPix);

    /// 温度 → 语义色（静态，供图例复用；<40 蓝绿渐深、40-55 黄、≥55 红）
    static QColor tempColor(float temp);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

private:
    void updateHover(int xPix);

    QVector<Cell> m_cells;
    QString m_hoverText;
    int m_hoverIdx = -1;
};

}  // namespace ens::ui
