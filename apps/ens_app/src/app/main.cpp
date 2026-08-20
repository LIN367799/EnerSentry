#include <QApplication>
#include <QMainWindow>
#include <QLabel>
#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>

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
