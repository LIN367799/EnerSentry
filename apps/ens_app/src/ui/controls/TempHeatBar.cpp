// src/ui/controls/TempHeatBar.cpp —— 簇最高温热力条实现（切片 40）。
#include "controls/TempHeatBar.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace ens::ui {

namespace {
/// 温度 → 梯度色：蓝(≤40) → 黄(40-50) → 红(≥60)，区间线性插值
QColor heatGradient(float t) {
    const float kLo = 40.0f, kMid = 50.0f, kHi = 60.0f;
    const QColor blue(0x2a, 0x6f, 0x97), yellow(0xd9, 0xa5, 0x2a), red(0xb4, 0x2f, 0x2f);
    if (t <= kLo) {
        // 蓝 → 冷区深蓝（更低温更暗蓝）
        return blue.darker(160 - static_cast<int>(120.0f * (1.0f - t / kLo)));
    }
    if (t < kMid) {
        const float p = (t - kLo) / (kMid - kLo);
        return QColor::fromRgbF(blue.redF() + (yellow.redF() - blue.redF()) * p,
                                blue.greenF() + (yellow.greenF() - blue.greenF()) * p,
                                blue.blueF() + (yellow.blueF() - blue.blueF()) * p);
    }
    if (t < kHi) {
        const float p = (t - kMid) / (kHi - kMid);
        return QColor::fromRgbF(yellow.redF() + (red.redF() - yellow.redF()) * p,
                                yellow.greenF() + (red.greenF() - yellow.greenF()) * p,
                                yellow.blueF() + (red.blueF() - yellow.blueF()) * p);
    }
    return red;
}
}  // namespace

TempHeatBar::TempHeatBar(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(56);
    setMouseTracking(true);
}

QColor TempHeatBar::tempColor(float temp) {
    return heatGradient(temp);
}

void TempHeatBar::setCells(const QVector<Cell>& cells) {
    m_cells = cells;
    m_hoverIdx = -1;
    m_hoverText.clear();
    update();
}

void TempHeatBar::updateHover(int xPix) {
    const int n = m_cells.size();
    int idx = -1;
    if (n > 0 && width() > 0) {
        idx = static_cast<int>(xPix * n / width());
        idx = qBound(0, idx, n - 1);
    }
    if (idx == m_hoverIdx) return;
    m_hoverIdx = idx;
    if (idx >= 0) {
        const Cell& c = m_cells[idx];
        m_hoverText = c.valid
            ? QStringLiteral("%1  %2 ℃").arg(c.label).arg(c.temp, 0, 'f', 1)
            : QStringLiteral("%1  -- ℃").arg(c.label);
    } else {
        m_hoverText.clear();
    }
    update();
}

void TempHeatBar::notifyMouseMoved(int xPix) {
    updateHover(xPix);
}

void TempHeatBar::mouseMoveEvent(QMouseEvent* e) {
    updateHover(e->pos().x());
    QWidget::mouseMoveEvent(e);
}

void TempHeatBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int n = m_cells.size();
    const int h = height() - 16;                     // 底部留悬停文本行
    if (h < 30) return;
    const double x0 = 0, y0 = 0, cw = (n > 0) ? width() / double(n) : width();

    // 标题（左上小字）
    p.setPen(QColor(0x9f, 0xb3, 0xc8));
    QFont t = font();
    t.setPointSizeF(9.0);
    p.setFont(t);
    p.drawText(QRectF(0, 0, width(), 14), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("簇最高温 ℃（≤40 蓝 · 40-50 黄 · ≥55 红）"));

    const double cellH = h - 16;
    for (int i = 0; i < n; ++i) {
        const Cell& c = m_cells[i];
        const double cx = x0 + i * cw + 2.0;
        const double cw2 = cw - 4.0;
        if (cw2 <= 0) continue;
        QRectF cellRect(cx, 14, cw2, cellH);
        if (!c.valid) {
            p.setPen(QPen(QColor(0x3a, 0x43, 0x4e), 1));
            p.setBrush(QColor(0x22, 0x28, 0x30));
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(heatGradient(c.temp));
        }
        p.drawRoundedRect(cellRect, 3, 3);
        // 标签
        p.setPen(QColor(0xe8, 0xed, 0xf2));
        p.drawText(cellRect, Qt::AlignCenter,
                   c.valid ? QStringLiteral("%1\n%2°").arg(c.label).arg(c.temp, 0, 'f', 0)
                           : c.label);
    }
    // 空态
    if (n == 0) {
        p.setPen(QColor(0x5a, 0x64, 0x70));
        p.drawText(QRectF(0, 14, width(), h - 14), Qt::AlignCenter,
                   QStringLiteral("暂无簇温度数据"));
    }

    // 悬停读值行（底部）
    p.setPen(QColor(0xe0, 0xc0, 0x60));
    p.drawText(QRectF(0, height() - 14, width(), 14), Qt::AlignLeft | Qt::AlignVCenter,
               m_hoverText);
}

}  // namespace ens::ui
