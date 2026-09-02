// src/ui/controls/CriticalAlarmDialog.cpp —— 严重告警弹窗实现（切片 37）。
#include "controls/CriticalAlarmDialog.h"
#include "ui_CriticalAlarmDialog.h"

#include <QDateTime>

namespace ens::ui {

namespace {
QString fmtTime(uint64_t epochMs) {
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(epochMs))
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}
QString fmtValue(double v) {
    return QString::number(v, 'f', 2);
}
}  // namespace

CriticalAlarmDialog::CriticalAlarmDialog(const ens::business::AlarmEvent& ev,
                                         const QString& pointName, QWidget* parent)
    : QDialog(parent), ui(new Ui::CriticalAlarmDialog) {
    ui->setupUi(this);
    setModal(false);                       // 非模态：不阻塞操作台（弹窗提示语义）
    setEvent(ev, pointName);
    connect(ui->btnClose, &QPushButton::clicked, this, &CriticalAlarmDialog::close);
}

CriticalAlarmDialog::~CriticalAlarmDialog() {
    delete ui;
}

void CriticalAlarmDialog::setEvent(const ens::business::AlarmEvent& ev,
                                   const QString& pointName) {
    ui->lblTriggerTime->setText(fmtTime(ev.triggerTime));
    const QString point = pointName.isEmpty()
        ? QString::number(ev.pointId)
        : QStringLiteral("%1 (id=%2)").arg(pointName).arg(ev.pointId);
    ui->lblPoint->setText(point);
    ui->lblValue->setText(fmtValue(ev.alarmValue));
    ui->lblThreshold->setText(fmtValue(ev.threshold));
    ui->lblDescription->setText(QString::fromStdString(ev.description));
}

}  // namespace ens::ui
