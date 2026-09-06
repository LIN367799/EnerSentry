// src/ui/common/WindowChrome.cpp
#include "common/WindowChrome.h"

#include "common/FramelessHelper.h"
#include "common/TitleBar.h"

#include <QWidget>

namespace ens::ui {

WindowChrome::WindowChrome(QWidget* target, TitleBar* titleBar, bool resizable,
                           const QIcon& appIcon)
    : m_helper(nullptr) {
    if (!target) return;
    target->setWindowFlags(target->windowFlags() | Qt::FramelessWindowHint);
    m_helper = new FramelessHelper(target, titleBar, target);
    m_helper->setResizable(resizable);
    m_helper->setMovable(true);
    m_helper->setSnapToMaximize(true);

    // 切片 46：setWindowFlags 重建 native window handle(HWND)，Qt 5.15 在
    // QMainWindow+setMenuWidget 路径不会自动把 windowIcon 写回新 HWND 的 HICON
    // → 任务栏图标退化空白。修复路径分两段：
    //   1) 此处立即 setWindowIcon：构造阶段 target 未 show，platformWindow 未建，
    //      Qt 只存 d->icon (内存态)，不会触发 WM_SETICON；但 offscreen 测试需要它
    //      才能在 show 前观察到 d->icon 非空。
    //   2) FramelessHelper::eventFilter 截到 QEvent::Show 后（platformWindow 已建），
    //      再调一次 setWindowIcon → 100% 触发 WM_SETICON → HICON 写回。
    // 注意：此处不能直接读 target->windowIcon() 兜底——重建瞬间已被 Qt 内部清空。
    if (!appIcon.isNull()) {
        target->setWindowIcon(appIcon);
        m_helper->setAppIcon(appIcon);
    }
}

WindowChrome::~WindowChrome() {
    // m_helper parent == target，target 析构时自动释放，无需手动
}

FramelessHelper* WindowChrome::helper() const {
    return m_helper.data();
}

void WindowChrome::applyDefaultLogo(TitleBar* tb) {
    if (tb) tb->setTitleIconPath(QStringLiteral(":/icons/app_logo.svg"));
}

}  // namespace ens::ui
