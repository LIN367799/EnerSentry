// src/ui/common/WindowChrome.cpp
#include "common/WindowChrome.h"

#include "common/FramelessHelper.h"
#include "common/TitleBar.h"

#include <QWidget>

namespace ens::ui {

WindowChrome::WindowChrome(QWidget* target, TitleBar* titleBar, bool resizable)
    : m_helper(nullptr) {
    if (!target) return;
    target->setWindowFlags(target->windowFlags() | Qt::FramelessWindowHint);
    m_helper = new FramelessHelper(target, titleBar, target);
    m_helper->setResizable(resizable);
    m_helper->setMovable(true);
    m_helper->setSnapToMaximize(true);
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
