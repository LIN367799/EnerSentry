// src/ui/common/theme.h —— L5 主题装载（SRS UI-01 / HLD-UI §1.2 / LLD-500 §6.3）。
// 加载 theme.qss（优先 :/qss/theme.qss 资源，其次外部文件路径）并应用为全局样式。
// High DPI 变量注入（--dpi-*，HLD-UI §4.4.2）留 5A.1.4；本函数仅做资源解析 + setStyleSheet。
#pragma once

#include <QString>

class QApplication;

namespace ens::ui {

/// @return true 成功应用；false qss 无法读取/为空（调用方自行处理回退）
bool applyTheme(QApplication* app, const QString& filePath = QString());

}  // namespace ens::ui
