// src/ui/common/TitleBar.h —— L5 自绘窗口标题栏（切片 45）。
// 设计动机：Qt 平台插件在 Windows 上给 QDialog/QMainWindow 渲染的原生标题栏
// 跟随系统主题，在"高对比度"等场景下出现"全红"等与 ENS 暗色主题冲突的视觉。
// 改用本组件接管标题栏：左 logo + 标题 + 右侧 min/max/close 按钮，整体走 theme.qss。
// 拖拽：mousePress/Move 内置 emit dragPressed(globalPos) 给 FramelessHelper；
//       双击：emit maximizeToggled(!isMaximized) 由消费者切换图标。
// 关闭语义：closeClicked 即可；Dialog reject / Widget close 由消费者决定。
#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QMouseEvent;
class QPushButton;

namespace ens::ui {

class TitleBar : public QWidget {
    Q_OBJECT
public:
    explicit TitleBar(QWidget* parent = nullptr);
    ~TitleBar() override;

    /// 标题文字（左侧 logo 后）
    void setTitle(const QString& title);
    QString title() const;

    /// 标题图标（默认 :/icons/app_logo.svg）
    void setTitleIconPath(const QString& qrcPath);

    /// 最小化/最大化按钮显隐（Dialog 经常只保留 close）
    void setMinimumVisible(bool v);
    void setMaximumVisible(bool v);

    /// 切换最大化按钮图标（最大化时显示还原图标，还原时显示最大化图标）
    void setMaximizeButtonChecked(bool checked);

    /// 推荐固定高度（构造时已 setFixedHeight 32）
    static int preferredHeight() { return 32; }

signals:
    /// 关闭按钮点击
    void closeClicked();
    /// 最小化按钮点击
    void minimizeClicked();
    /// 最大化/还原切换（参数：true=要求最大化，false=要求还原；消费者自行判定当前态）
    void maximizeToggled(bool wantMaximize);
    /// 拖拽开始（globalPos 来自 mousePress 的 globalPosition）
    void dragPressed(QPoint globalPos);
    /// 双击标题栏（消费者接 maximizeToggled 同样处理）
    void doubleClicked();

protected:
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    void setupUi();
    void updateMaximizeIcon();

    QPointer<QLabel>      m_logo;
    QPointer<QLabel>      m_title;
    QPointer<QPushButton> m_minBtn;
    QPointer<QPushButton> m_maxBtn;
    QPointer<QPushButton> m_closeBtn;

    QString m_titleIconPath = QStringLiteral(":/icons/app_logo.svg");
    bool    m_maxChecked    = false;
    QPoint  m_pressGlobal;
    bool    m_dragging      = false;
};

}  // namespace ens::ui
