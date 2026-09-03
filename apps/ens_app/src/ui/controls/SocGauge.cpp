// src/ui/controls/SocGauge.cpp —— SOC 仪表盘实现（切片 40）。
#include "controls/SocGauge.h"

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QtMath>

namespace ens::ui {

namespace {
// 240° 弧（数学角：-210° 起 → +30° 止，留 150° 开口向下）
constexpr double kStartDeg  = -210.0;
constexpr double kSpanDeg   = 240.0;
constexpr int    kArcWidth  = 18;

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

/// 三段语义色（SOC：<20 红 / 20-50 黄 / ≥50 绿）在进度 p∈[0,1] 上插值
QColor socColorAt(double p) {
    if (p < 0.20) {
        // 0 → 深红渐红
        return QColor(0x7a, 0x1f, 0x1f).darker(100 + 40 * (1.0 - p / 0.20));
    }
    if (p < 0.50) {
        return QColor(0xb0, 0x8a, 0x1e);   // 黄段
    }
    return QColor(0x1f, 0x7a, 0x3d);       // 绿段
}
}  // namespace

SocGauge::SocGauge(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 130);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void SocGauge::setValue(double soc) {
    double v = qBound(0.0, soc, 100.0);
    if (qAbs(v - m_value) < 0.01) return;
    m_value = v;
    update();
}

void SocGauge::setTitle(const QString& t) {
    m_title = t;
    update();
}

void SocGauge::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const double w = width(), h = height();
    const QPointF c(w / 2.0, h / 2.0 + 6.0);
    const double r = qMin(w, h) / 2.0 - kArcWidth - 4.0;
    if (r < 8.0) return;

    QRectF arcRect(c.x() - r, c.y() - r, 2 * r, 2 * r);
    const QRectF arcRectOut = arcRect.adjusted(-kArcWidth / 2.0, -kArcWidth / 2.0,
                                               kArcWidth / 2.0, kArcWidth / 2.0);

    // 背景整环（灰）
    p.setPen(QPen(QColor(0x2a, 0x31, 0x3a), kArcWidth, Qt::SolidLine, Qt::FlatCap));
    p.drawArc(arcRectOut, static_cast<int>(kStartDeg * 16), static_cast<int>(kSpanDeg * 16));

    // 前景值弧（语义色按当前值亮度 + 白描边强调）
    const double frac = clamp01(m_value / 100.0);
    if (frac > 0.001) {
        QColor fill = socColorAt(frac);
        p.setPen(QPen(fill.lighter(120), kArcWidth - 2, Qt::SolidLine, Qt::FlatCap));
        p.drawArc(arcRectOut.adjusted(1, 1, -1, -1),
                  static_cast<int>(kStartDeg * 16),
                  static_cast<int>(kSpanDeg * frac * 16));
    }

    // 中心值文本
    p.setPen(QColor(0xe8, 0xed, 0xf2));
    QFont vf = font();
    vf.setPointSizeF(qMax(11.0, r * 0.30));
    vf.setBold(true);
    p.setFont(vf);
    p.drawText(QRectF(c.x() - r * 0.8, c.y() - r * 0.42, r * 1.6, r * 0.42),
               Qt::AlignCenter,
               QStringLiteral("%1%").arg(m_value, 0, 'f', 0));

    p.setPen(QColor(0x9f, 0xb3, 0xc8));
    QFont tf = font();
    tf.setPointSizeF(qMax(9.0, r * 0.16));
    p.setFont(tf);
    p.drawText(QRectF(c.x() - r * 0.9, c.y() - r * 0.05, r * 1.8, r * 0.24),
               Qt::AlignCenter, m_title);
}

}  // namespace ens::ui
