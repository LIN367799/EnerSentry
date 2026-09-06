// src/ui/common/FramelessHelper.h —— L5 通用 Frameless 窗口装饰接管（切片 45）。
// 设计动机：去除 Qt 平台插件在 Windows 上默认渲染的原生标题栏（与 ENS 暗色主题冲突，
//          "高对比度"模式下出现"全红"等不一致视觉）。
// 用法：
//   auto* tb = new TitleBar(target);
//   target->layout()->addWidget(tb);   // 顶部占 32px
//   target->setWindowFlags(target->windowFlags() | Qt::FramelessWindowHint);
//   auto* helper = new FramelessHelper(target, tb, target);
//   helper->setResizable(true);       // Dialog 一般传 false
//   connect(tb, &TitleBar::closeClicked, target, &QWidget::close);
//   connect(tb, &TitleBar::minimizeClicked, target, &QWidget::showMinimized);
//   connect(tb, &TitleBar::maximizeToggled, target, [target](bool m) {
//       m ? target->showMaximized() : target->showNormal();
//   });
//   connect(helper, &FramelessHelper::resized, target, [target](QSize){ ... });
//   connect(helper, &FramelessHelper::moved, target, [target](QPoint){ ... });
//
// 实现：
//   * 边缘 6px 捕获区 → mousePress 记录 edges；mouseMove 调整 size
//   * 拖拽区（TitleBar）→ mousePress 记录 offset；mouseMove 调 window->move
//   * Aero Snap：mouseRelease 时若 globalY<=0 → showMaximized
//   * OS 原生 startSystemMove/startSystemResize 优先（Qt 5.15 Windows 平台）；
//     offscreen 平台插件下用兜底 move/resize。
#pragma once

#include <QIcon>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QSize>
#include <Qt>

class QEvent;
class QMouseEvent;
class QWidget;

namespace ens::ui {

class FramelessHelper : public QObject {
    Q_OBJECT
public:
    /// @param target    目标窗口（必须是 QWidget 子类，有 top-level windowHandle）
    /// @param dragRegion 拖拽热区（一般传 TitleBar；nullptr=全窗可拖）
    /// @param parent     QObject 父对象（一般传 target，自动随 target 析构）
    explicit FramelessHelper(QWidget* target, QWidget* dragRegion = nullptr,
                             QObject* parent = nullptr);
    ~FramelessHelper() override;

    FramelessHelper(const FramelessHelper&) = delete;
    FramelessHelper& operator=(const FramelessHelper&) = delete;

    // ── 配置 ──
    /// 边缘捕获宽度（像素，默认 6）
    void setBorderWidth(int w);
    int  borderWidth() const { return m_borderWidth; }

    /// 允许边缘缩放（Dialog 经常关闭）
    void setResizable(bool on);
    bool resizable() const { return m_resizable; }

    /// 允许拖拽（一般保持 true；锁定屏幕可关）
    void setMovable(bool on);
    bool movable() const { return m_movable; }

    /// 拖到屏幕顶部自动最大化（Windows Aero Snap，1px 容差）
    void setSnapToMaximize(bool on);
    bool snapToMaximize() const { return m_snapToMaximize; }

    /// 切片 46：Show 后 reapply 应用图标。
    /// WindowChrome 在 setWindowFlags 重建 HWND 后调一次本 setter；FramelessHelper
    /// 在首次收到 QEvent::Show 时（platformWindow 已建、native HWND 存在）强制
    /// target->setWindowIcon(m_appIcon)，确保 WM_SETICON 真正写入 HICON。
    /// Qt 内存态写回的 setWindowIcon 在 target 未 show 时只存 d->icon 不触发
    /// platformWindow->setIcon()，而 QMainWindow + setMenuWidget + 二次重建这条路径
    /// 上 Qt show 内部 apply 走 menubar 分支、不会再 apply widget 自己的 windowIcon
    /// —— offscreen 测试看不到该分支差异，假绿。Show 事件后 reapply 100% 生效。
    void setAppIcon(const QIcon& icon) { m_appIcon = icon; }
    QIcon appIcon() const { return m_appIcon; }

    // ── 测试/调试 API ──
    /// 命中测试：给定 target 内部 localPos，返回 0=N 1=S 2=E 3=W 4=NW 5=NE 6=SW 7=SE 8=无
    int hitTestZone(const QPoint& localPos) const;
    static Qt::Edges zoneToEdges(int zone);

signals:
    /// 拖拽导致窗口移动
    void moved(const QPoint& newTopLeft);
    /// 边缘缩放导致窗口尺寸变化
    void resized(const QSize& newSize);
    /// Aero Snap 触发（拖到顶部自动最大化）
    void snapMaximized();
    /// 拖拽开始（消费者可在此保存当前位置做后续计算）
    void dragStarted(const QPoint& globalPos);
    /// 拖拽结束
    void dragFinished();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    bool handleMouseButtonPress(QMouseEvent* e);
    bool handleMouseMove(QMouseEvent* e);
    bool handleMouseButtonRelease(QMouseEvent* e);

    /// 尝试走 Qt 5.15+ 原生 startSystemResize/Move；offscreen 平台下返回 false
    bool trySystemResize(Qt::Edges edges);
    bool trySystemMove();

    QPointer<QWidget> m_target;
    QPointer<QWidget> m_dragRegion;
    int  m_borderWidth   = 6;
    bool m_resizable     = true;
    bool m_movable       = true;
    bool m_snapToMaximize = true;
    QIcon m_appIcon;                     // 切片 46：Show 后 reapply 应用图标
    bool  m_iconReapplied = false;       // 切片 46：Show reapply 一次性闸

    // 拖拽状态
    bool         m_dragging   = false;
    QPoint       m_pressGlobal;     // mousePress 时的全局坐标
    QPoint       m_pressTopLeft;    // mousePress 时 window 几何左上
    Qt::Edges    m_resizeEdges = Qt::Edges{};
    QSize        m_pressSize;       // 缩放起始 size
};

}  // namespace ens::ui
