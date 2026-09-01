// src/ui/views/placeholder_view.cpp —— 占位视图实现（纯代码构造，无需 .ui）。
#include "views/placeholder_view.h"

#include <QLabel>
#include <QVBoxLayout>

namespace ens::ui {

PlaceholderView::PlaceholderView(const QString& title, const QString& plan, QWidget* parent)
    : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    auto* titleLbl = new QLabel(title, this);
    titleLbl->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600; color: #9fb3c8;"));
    lay->addWidget(titleLbl);
    auto* planLbl = new QLabel(plan, this);
    planLbl->setWordWrap(true);
    planLbl->setStyleSheet(QStringLiteral("color: #8b949e;"));
    lay->addWidget(planLbl);
    lay->addStretch(1);
}

}  // namespace ens::ui
