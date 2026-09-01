// src/ui/common/theme.cpp —— L5 主题装载实现。
#include "common/theme.h"

#include <QApplication>
#include <QFile>

namespace ens::ui {

bool applyTheme(QApplication* app, const QString& filePath) {
    if (!app) return false;

    QString qss;
    // 优先资源路径（resources.qrc 编译入可执行文件，部署自足）
    QFile res(QStringLiteral(":/qss/theme.qss"));
    if (res.open(QIODevice::ReadOnly)) {
        qss = QString::fromUtf8(res.readAll());
    } else if (!filePath.isEmpty()) {
        // 外部文件路径回退（开发期热改样式用）
        QFile f(filePath);
        if (f.open(QIODevice::ReadOnly)) {
            qss = QString::fromUtf8(f.readAll());
        }
    }
    if (qss.isEmpty()) return false;

    app->setStyleSheet(qss);
    return true;
}

}  // namespace ens::ui
