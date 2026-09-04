// src/ui/common/FramelessHelper.cpp —— 切片 45 Frameless 接管实现。
// 关键设计：所有 mouse 处理直接走 eventFilter 内联；按 obj == m_target 判定边缘
// 缩放热区，按 obj == m_dragRegion 判定拖拽热区；offscreen 平台插件下 Qt 的
// startSystemMove/Resize 直接返回 false（无 nativeEvent），走兜底 move/resize。
#include "common/FramelessHelper.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPoint>
#include <QPointF>
#include <QWidget>
#include <QWindow>

namespace ens::ui {

FramelessHelper::FramelessHelper(QWidget* target, QWidget* dragRegion, QObject* parent)
    : QObject(parent), m_target(target), m_dragRegion(dragRegion) {
    if (m_target) {
        m_target->installEventFilter(this);
    }
    if (m_dragRegion && m_dragRegion != m_target) {
        m_dragRegion->installEventFilter(this);
    }
}

FramelessHelper::~FramelessHelper() {
    if (m_target) m_target->removeEventFilter(this);
    if (m_dragRegion && m_dragRegion != m_target) m_dragRegion->removeEventFilter(this);
}

void FramelessHelper::setBorderWidth(int w) {
    m_borderWidth = (w < 1) ? 1 : (w > 32 ? 32 : w);
}

void FramelessHelper::setResizable(bool on) { m_resizable = on; }
void FramelessHelper::setMovable(bool on)   { m_movable = on; }
void FramelessHelper::setSnapToMaximize(bool on) { m_snapToMaximize = on; }

int FramelessHelper::hitTestZone(const QPoint& localPos) const {
    if (!m_target || !m_resizable) return 8;
    const QSize sz = m_target->size();
    if (sz.isEmpty()) return 8;
    const int w = qMin(m_borderWidth, sz.height() / 2);
    const int qw = qMin(m_borderWidth, sz.width()  / 2);
    const bool onLeft   = localPos.x() <= w;
    const bool onRight  = localPos.x() >= sz.width()  - qw;
    const bool onTop    = localPos.y() <= w;
    const bool onBottom = localPos.y() >= sz.height() - qw;
    if (onTop && onLeft)        return 4;   // NW
    if (onTop && onRight)       return 5;   // NE
    if (onBottom && onLeft)     return 6;   // SW
    if (onBottom && onRight)    return 7;   // SE
    if (onTop)                  return 0;   // N
    if (onBottom)               return 1;   // S
    if (onLeft)                 return 2;   // W
    if (onRight)                return 3;   // E
    return 8;
}

Qt::Edges FramelessHelper::zoneToEdges(int zone) {
    switch (zone) {
        case 0: return Qt::TopEdge;
        case 1: return Qt::BottomEdge;
        case 2: return Qt::LeftEdge;
        case 3: return Qt::RightEdge;
        case 4: return Qt::TopEdge | Qt::LeftEdge;
        case 5: return Qt::TopEdge | Qt::RightEdge;
        case 6: return Qt::BottomEdge | Qt::LeftEdge;
        case 7: return Qt::BottomEdge | Qt::RightEdge;
        default: return Qt::Edges{};
    }
}

bool FramelessHelper::eventFilter(QObject* obj, QEvent* e) {
    if (!m_target) return false;
    const bool isTarget  = (obj == m_target);
    const bool isDragReg = (obj == m_dragRegion) && (m_dragRegion && m_dragRegion != m_target);
    if (!isTarget && !isDragReg) return false;

    switch (e->type()) {
        case QEvent::MouseButtonPress: {
            auto* me = static_cast<QMouseEvent*>(e);
            if (me->button() != Qt::LeftButton) return false;
            m_pressGlobal  = me->globalPos();
            m_pressTopLeft = m_target->frameGeometry().topLeft();
            m_pressSize    = m_target->size();
            m_dragging     = false;
            m_resizeEdges  = Qt::Edges{};

            if (isDragReg && m_movable) {
                m_dragging = true;
                emit dragStarted(m_pressGlobal);
                return false;   // 让 TitleBar 自身的 mousePress 继续走（它会 emit dragPressed）
            }
            if (isTarget && m_resizable) {
                const int zone = hitTestZone(me->pos());
                if (zone != 8) {
                    m_resizeEdges = zoneToEdges(zone);
                    if (!trySystemResize(m_resizeEdges)) {
                        m_dragging = true;
                    }
                    return true;
                }
            }
            return false;
        }
        case QEvent::MouseMove: {
            auto* me = static_cast<QMouseEvent*>(e);
            if (!(me->buttons() & Qt::LeftButton) || !m_target) return false;
            if (m_dragging && m_movable && m_resizeEdges == Qt::Edges{}) {
                const QPoint delta = me->globalPos() - m_pressGlobal;
                m_target->move(m_pressTopLeft + delta);
                emit moved(m_pressTopLeft + delta);
                return true;
            }
            if (m_resizeEdges == Qt::Edges{} || !m_resizable) return false;
            const QPoint g = me->globalPos();
            QRect geom = m_target->frameGeometry();
            if (m_resizeEdges & Qt::LeftEdge) {
                const int nx = qMin(g.x(), geom.right() - 100);
                const int dw = geom.x() - nx;
                geom.setX(nx);
                geom.setWidth(geom.width() + dw);
            }
            if (m_resizeEdges & Qt::RightEdge) {
                geom.setWidth(qMax(100, g.x() - geom.x()));
            }
            if (m_resizeEdges & Qt::TopEdge) {
                const int ny = qMin(g.y(), geom.bottom() - 60);
                const int dh = geom.y() - ny;
                geom.setY(ny);
                geom.setHeight(geom.height() + dh);
            }
            if (m_resizeEdges & Qt::BottomEdge) {
                geom.setHeight(qMax(60, g.y() - geom.y()));
            }
            m_target->setGeometry(geom);
            emit resized(geom.size());
            return true;
        }
        case QEvent::MouseButtonRelease: {
            auto* me = static_cast<QMouseEvent*>(e);
            if (me->button() != Qt::LeftButton) return false;
            const bool wasDragging = m_dragging;
            const bool wasResizing = (m_resizeEdges != Qt::Edges{});
            m_dragging = false;
            m_resizeEdges = Qt::Edges{};
            if ((wasDragging || wasResizing) && m_snapToMaximize && m_target) {
                if (me->globalPos().y() <= 1) {
                    m_target->showMaximized();
                    emit snapMaximized();
                }
            }
            if (wasDragging) emit dragFinished();
            return false;
        }
        default:
            break;
    }
    return false;
}

bool FramelessHelper::trySystemResize(Qt::Edges edges) {
    if (!m_target) return false;
    QWindow* w = m_target->windowHandle();
    if (!w) return false;
    w->startSystemResize(edges);
    return true;
}

bool FramelessHelper::trySystemMove() {
    if (!m_target) return false;
    QWindow* w = m_target->windowHandle();
    if (!w) return false;
    w->startSystemMove();
    return true;
}

}  // namespace ens::ui
