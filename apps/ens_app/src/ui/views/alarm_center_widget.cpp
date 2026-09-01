// src/ui/views/alarm_center_widget.cpp —— 告警中心骨架实现。
#include "views/alarm_center_widget.h"
#include "ui_AlarmCenterWidget.h"

#include "AlarmEngine.h"

#include <QDateTime>
#include <QHeaderView>

namespace ens::ui {

namespace {
QString levelText(ens::business::AlarmLevel l) {
    using ens::business::AlarmLevel;
    switch (l) {
        case AlarmLevel::Info:     return QStringLiteral("Info");
        case AlarmLevel::Warning:  return QStringLiteral("Warning");
        case AlarmLevel::Critical: return QStringLiteral("Critical");
    }
    return QStringLiteral("?");
}
}  // namespace

AlarmCenterWidget::AlarmCenterWidget(ens::business::AlarmEngine* alarm, QWidget* parent)
    : QWidget(parent), ui(new Ui::AlarmCenterWidget), m_alarm(alarm) {
    ui->setupUi(this);

    m_model = new QStandardItemModel(this);
    m_model->setHorizontalHeaderLabels(
        {QStringLiteral("时间"), QStringLiteral("级别"), QStringLiteral("测点"),
         QStringLiteral("值"), QStringLiteral("阈值"), QStringLiteral("描述")});
    ui->tableAlarms->setModel(m_model);
    ui->tableAlarms->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->tableAlarms->verticalHeader()->setVisible(false);

    if (m_alarm) {
        // AlarmEvent 已 qRegisterMetaType（AlarmEngine.cpp）；AlarmEngine 在告警线程，
        // 此处自动 QueuedConnection 跨线程投递
        connect(m_alarm, &ens::business::AlarmEngine::alarmTriggered,
                this, &AlarmCenterWidget::onAlarmTriggered);
    }

    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &AlarmCenterWidget::onRefreshActive);
    m_timer.start();
}

AlarmCenterWidget::~AlarmCenterWidget() {
    m_timer.stop();
    delete ui;
}

void AlarmCenterWidget::onAlarmTriggered(const ens::business::AlarmEvent& ev) {
    appendRow(ev);
}

void AlarmCenterWidget::appendRow(const ens::business::AlarmEvent& ev) {
    const int row = m_model->rowCount();
    const QString ts = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(ev.triggerTime)).toString(QStringLiteral("HH:mm:ss"));
    m_model->setItem(row, 0, new QStandardItem(ts));
    auto* lvl = new QStandardItem(levelText(ev.level));
    // 告警语义色（UI-02，不可随主题漂移）：Critical 红 / Warning 黄 / Info 蓝
    QColor bg;
    switch (ev.level) {
        case ens::business::AlarmLevel::Critical: bg = QColor(0x7a, 0x1f, 0x1f); break;
        case ens::business::AlarmLevel::Warning:  bg = QColor(0x6b, 0x55, 0x1a); break;
        default:                                  bg = QColor(0x1a, 0x33, 0x55); break;
    }
    lvl->setBackground(bg);
    m_model->setItem(row, 1, lvl);
    m_model->setItem(row, 2, new QStandardItem(QString::number(ev.pointId)));
    m_model->setItem(row, 3, new QStandardItem(QString::number(static_cast<double>(ev.alarmValue), 'f', 2)));
    m_model->setItem(row, 4, new QStandardItem(QString::number(static_cast<double>(ev.threshold), 'f', 2)));
    m_model->setItem(row, 5, new QStandardItem(QString::fromStdString(ev.description)));
}

void AlarmCenterWidget::onRefreshActive() {
    if (m_alarm) {
        ui->lblActive->setText(QString::number(m_alarm->activeAlarmCount()));
    }
}

}  // namespace ens::ui
