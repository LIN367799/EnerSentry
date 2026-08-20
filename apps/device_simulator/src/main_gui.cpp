#include <QApplication>
#include <QMainWindow>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

// BUILD-0 stub：仅验证「Qt GUI 可执行 + 窗口能开 + 按钮可点」。
// 后续替换为 ENS-SIM-IMP §5 / §10 的 SimulatorMainWindow（设备树 + 寄存器表 + 故障面板）。
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QMainWindow w;
    w.setWindowTitle("EnerSentry Device Simulator");
    auto* central = new QWidget(&w);
    auto* lay = new QVBoxLayout(central);
    auto* btn = new QPushButton("启动从站 (stub)");
    lay->addWidget(btn);
    lay->addWidget(new QLabel("Device Simulator boot OK — BUILD-0 passed"));
    w.setCentralWidget(central);
    w.resize(600, 400);
    w.show();
    return app.exec();
}
