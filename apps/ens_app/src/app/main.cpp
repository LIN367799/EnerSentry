#include <QApplication>
#include <QMainWindow>
#include <QLabel>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMetaType>
#include <QVBoxLayout>

// ens_protocol PUBLIC include src/protocol — 此处直接 include 即可
#include "ModbusEngine.h"   // Q_DECLARE_METATYPE(ModbusResponse) 来源

// BUILD-0 暂用 stub 登录框：直接返回 Accepted，
// 后续替换为 ENS-LLD-500 §7 的 LoginDialog（内嵌 LoginWidget）。
class LoginDialog : public QDialog {
public:
    explicit LoginDialog(QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle("EnerSentry - Login (stub)");
        auto* lay = new QVBoxLayout(this);
        lay->addWidget(new QLabel("Login stub — BUILD-0"));
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok);
        lay->addWidget(bb);
        connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    }
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // 注册 ModbusResponse 到 Qt 元类型系统:跨线程信号(QueuedConnection)
    // 在 ModbusEngine worker 线程 → 主线程 slot 时,Qt 必须知道类型布局。
    // 未注册时信号跨线程静默丢弃(用户截图 V2 坑)。
    qRegisterMetaType<ens::protocol::ModbusResponse>("ens::protocol::ModbusResponse");

    // 启动顺序（ENS-LLD-500 §7 / ENS-DEV-ARCH §3.5）：先登录，成功再显主窗
    LoginDialog dlg;
    if (dlg.exec() != QDialog::Accepted) return 0;

    QMainWindow w;
    w.setWindowTitle("EnerSentry");
    w.setCentralWidget(new QLabel("EnerSentry boot OK — BUILD-0 passed"));
    w.resize(800, 600);
    w.show();
    return app.exec();
}
