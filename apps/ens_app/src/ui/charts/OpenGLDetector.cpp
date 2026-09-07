// src/ui/charts/OpenGLDetector.cpp —— OpenGL 探测实现。
#include "charts/OpenGLDetector.h"

#include <QGuiApplication>
#include <QOpenGLContext>

#include "qcustomplot.h"

namespace ens::ui {

bool OpenGLDetector::isOpenGlAvailable() {
    // S47：offscreen 平台（CI / 无窗口测试环境）必须走软件渲染。
    //   QOpenGLContext::create() 在 offscreen 下同样会返回 true（ANGLE 软件 GL），
    //   于是 setOpenGl(true) 生效；但 offscreen 没有真实 surface，QCustomPlot 的
    //   QOpenGLWidget 视口会在渲染/析构时崩溃（实测 #savePng 2.8s 异常退出、
    //   MainWindow 七视图用例崩溃）。此处显式禁用，等价于"无 GL 驱动"的软件渲染分支。
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) return false;

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
