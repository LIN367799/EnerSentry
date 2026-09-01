// src/ui/charts/OpenGLDetector.h —— L5 渲染 OpenGL 兼容性探测（ENS-LLD-503 / HLD-UI §4.3）。
// 工控机无独显/远程桌面环境自动回退软件渲染，防 QCustomPlot setOpenGl 异常。
#pragma once

class QCustomPlot;

namespace ens::ui {

class OpenGLDetector {
public:
    /// 探测当前平台能否创建 OpenGL 上下文（无独显/远程会话通常 false）
    static bool isOpenGlAvailable();

    /// 可用则开启 QCustomPlot OpenGL 加速，返回是否已启用
    static bool applyTo(QCustomPlot* plot);
};

}  // namespace ens::ui
