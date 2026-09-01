// src/ui/views/placeholder_view.h —— 通用占位视图（切片 19：7 视图骨架中未实装的视图）。
// 仅显示视图名 + 规划说明，保证 CentralStack 7 视图可切换不崩（Tier 3 验收项）。
#pragma once

#include <QWidget>

namespace ens::ui {

class PlaceholderView : public QWidget {
    Q_OBJECT
public:
    explicit PlaceholderView(const QString& title, const QString& plan, QWidget* parent = nullptr);
};

}  // namespace ens::ui
