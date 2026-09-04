// src/ui/common/WindowChrome.h —— L5 薄封装：一行代码把任意 QWidget 变 Frameless（切片 45）。
// 内部：setWindowFlags(Frameless) + 构造 FramelessHelper(target, titleBar, target)。
// 不持有 TitleBar 生命周期（由 target->layout 拥有）；helper 随 target 析构。
// Dialog 默认不可缩放（resizable=false）；MainWindow 传 true。
//
// 注意：本头仅前向声明 FramelessHelper；helper() 返回裸指针（避免 QPointer<T> 要求
// T 完整类型带来的 include 循环）。
#pragma once

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
    WindowChrome(QWidget* target, TitleBar* titleBar, bool resizable = true);
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
