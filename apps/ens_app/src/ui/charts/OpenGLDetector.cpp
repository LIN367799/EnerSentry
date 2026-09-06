// src/ui/charts/OpenGLDetector.cpp —— OpenGL 探测实现。
#include "charts/OpenGLDetector.h"

#include <QOpenGLContext>

#include "qcustomplot.h"

namespace ens::ui {

bool OpenGLDetector::isOpenGlAvailable() {
    // 尝试创建 OpenGL 上下文：失败（无 GL 驱动/远程会话禁用）→ 软件渲染
    QOpenGLContext ctx;
    return ctx.create();
}

bool OpenGLDetector::applyTo(QCustomPlot* plot) {
#ifdef QCUSTOMPLOT_USE_OPENGL
    if (!plot || !isOpenGlAvailable()) return false;
    plot->setOpenGl(true);
    return plot->openGl();
#else
    (void)plot;
    return false;
#endif
}

}  // namespace ens::ui
