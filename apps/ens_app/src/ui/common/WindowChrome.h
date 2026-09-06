// src/ui/common/WindowChrome.h —— L5 薄封装：一行代码把任意 QWidget 变 Frameless（切片 45）。
// 内部：setWindowFlags(Frameless) + 构造 FramelessHelper(target, titleBar, target)。
// 不持有 TitleBar 生命周期（由 target->layout 拥有）；helper 随 target 析构。
// Dialog 默认不可缩放（resizable=false）；MainWindow 传 true。
//
// 切片 46：WindowChrome 内部 setWindowFlags 会重建 native window handle(HWND)，
// 而 Qt 5.15 在 QMainWindow + setMenuWidget(自绘 TitleBar) 路径上重建后不会把
// QWidget::windowIcon() 重新 apply 到新 HWND → 任务栏图标退化为 Windows 默认空白。
// 修复：构造时传入 appIcon，setWindowFlags 之后强制 target->setWindowIcon(appIcon)
// 再次写回 native（触发 WM_SETICON，任务栏图标恢复）。
//
// 注意：本头仅前向声明 FramelessHelper；helper() 返回裸指针（避免 QPointer<T> 要求
// T 完整类型带来的 include 循环）。
#pragma once

#include <QIcon>
#include <QPointer>

class QWidget;

namespace ens::ui {

class FramelessHelper;
class TitleBar;

class WindowChrome {
public:
    /// @param target      目标窗（QDialog/QWidget/...）
    /// @param titleBar    自绘标题栏（必须已 setParent(target) 并 addWidget 到 target 布局顶部）
    /// @param resizable   Dialog=false / MainWindow=true
    /// @param appIcon     任务栏/标题栏图标；传非空会在 setWindowFlags 重建后强制 reapply
    ///                    （切片 46：修复 QMainWindow+menuWidget 路径任务栏图标丢失）
    WindowChrome(QWidget* target, TitleBar* titleBar, bool resizable = true,
                 const QIcon& appIcon = QIcon());
    ~WindowChrome();

    WindowChrome(const WindowChrome&) = delete;
    WindowChrome& operator=(const WindowChrome&) = delete;

    FramelessHelper* helper() const;

    /// 工具：把 titleBar->titleIcon 设置为默认 ENS logo（:/icons/app_logo.svg）
    static void applyDefaultLogo(TitleBar* tb);

private:
    QPointer<FramelessHelper> m_helper;
};

}  // namespace ens::ui
