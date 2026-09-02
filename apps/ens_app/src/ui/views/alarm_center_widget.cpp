// src/ui/views/alarm_center_widget.cpp —— 告警中心完整版实现（切片 36）。
#include "views/alarm_center_widget.h"
#include "ui_AlarmCenterWidget.h"

#include "common/CsvWriter.h"
#include "AlarmEngine.h"
#include "PointTable.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QMessageBox>

#include <algorithm>
#include <filesystem>

namespace ens::ui {

namespace {

constexpr int kIdRole = Qt::UserRole;   // 行数据角色：alarm id

QString levelText(int level) {
    switch (level) {
        case 0:  return QStringLiteral("Info");
        case 1:  return QStringLiteral("Warning");
        case 2:  return QStringLiteral("Critical");
    }
    return QStringLiteral("?");
}
QString statusText(int status) {
    switch (status) {
        case 0:  return QStringLiteral("Active");
        case 1:  return QStringLiteral("Confirmed");
        case 2:  return QStringLiteral("Recovered");
    }
    return QStringLiteral("?");
}
QColor levelColor(int level) {
    // 告警语义色（UI-02，不随主题漂移）：Critical 红 / Warning 黄 / Info 蓝
    switch (level) {
        case 2:  return QColor(0x7a, 0x1f, 0x1f);
        case 1:  return QColor(0x6b, 0x55, 0x1a);
        default: return QColor(0x1a, 0x33, 0x55);
    }
}
QString fmtTime(uint64_t epochMs) {
    if (epochMs == 0) return QStringLiteral("-");
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(epochMs))
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}
QString fmtValue(double v) {
    return QString::number(v, 'f', 2);
}
QString listRangeText(uint64_t ms) {
    // comboRange data：毫秒数；0 表示"全部"
    if (ms == 0) return QStringLiteral("全部");
    if (ms == 3600ULL * 1000)        return QStringLiteral("最近 1 小时");
    if (ms == 24ULL * 3600 * 1000)   return QStringLiteral("最近 24 小时");
    if (ms == 7ULL * 24 * 3600 * 1000) return QStringLiteral("最近 7 天");
    return QStringLiteral("?");
}

}  // namespace

AlarmCenterWidget::AlarmCenterWidget(ens::business::AlarmEngine* alarm,
                                     ens::datahub::IAlarmAccess* access,
                                     const std::shared_ptr<ens::protocol::PointTable>& pt,
                                     std::function<QString()> currentUser,
                                     QWidget* parent)
    : QWidget(parent), ui(new Ui::AlarmCenterWidget), m_alarm(alarm), m_access(access),
      m_pt(pt), m_currentUser(std::move(currentUser)) {
    ui->setupUi(this);

    m_model = new QStandardItemModel(this);
    m_model->setHorizontalHeaderLabels(
        {QStringLiteral("触发时间"), QStringLiteral("级别"), QStringLiteral("状态"),
         QStringLiteral("测点"), QStringLiteral("名称"), QStringLiteral("值"),
         QStringLiteral("阈值"), QStringLiteral("确认人"), QStringLiteral("确认时间"),
         QStringLiteral("恢复时间"), QStringLiteral("描述")});
    ui->tableAlarms->setModel(m_model);
    QHeaderView* hh = ui->tableAlarms->horizontalHeader();
    hh->setSectionResizeMode(QHeaderView::Interactive);
    hh->setSectionResizeMode(10, QHeaderView::Stretch);   // 描述列拉伸
    hh->resizeSection(0, 150);
    hh->resizeSection(1, 70);
    hh->resizeSection(2, 80);
    ui->tableAlarms->verticalHeader()->setVisible(false);

    // 筛选工具条
    fillLevelsAndStatus();
    fillPoints(m_pt);
    const QList<uint64_t> ranges = {
        3600ULL * 1000, 24ULL * 3600 * 1000, 7ULL * 24 * 3600 * 1000, 0ULL};
    for (uint64_t ms : ranges) {
        ui->comboRange->addItem(listRangeText(ms), QVariant(static_cast<qlonglong>(ms)));
    }
    ui->comboRange->setCurrentIndex(1);   // 默认最近 24 小时

    connect(ui->btnQuery, &QPushButton::clicked, this, &AlarmCenterWidget::onQueryClicked);
    connect(ui->btnConfirm, &QPushButton::clicked, this, &AlarmCenterWidget::onConfirmClicked);
    connect(ui->btnExport, &QPushButton::clicked, this, &AlarmCenterWidget::onExportClicked);

    if (m_alarm) {
        // AlarmEngine 与 UI 同线程（主线程）→ direct；AlarmEvent 已注册 metatype（兼容 queued）
        connect(m_alarm, &ens::business::AlarmEngine::alarmTriggered,
                this, &AlarmCenterWidget::onAlarmTriggered);
    }
    if (m_currentUser) {
        // 无当前用户回调（未登录/CLI）→ 确认禁用
    } else {
        ui->btnConfirm->setEnabled(false);
    }

    m_activeTimer.setInterval(1000);
    connect(&m_activeTimer, &QTimer::timeout, this, &AlarmCenterWidget::onRefreshActive);
    m_activeTimer.start();

    m_debounceTimer.setSingleShot(true);
    m_debounceTimer.setInterval(1000);
    connect(&m_debounceTimer, &QTimer::timeout, this, &AlarmCenterWidget::onQueryClicked);

    if (m_access) onQueryClicked();   // 首查（alarm 库未启用时为空态，不崩）
}

AlarmCenterWidget::~AlarmCenterWidget() {
    m_activeTimer.stop();
    m_debounceTimer.stop();
    delete ui;
}

void AlarmCenterWidget::fillLevelsAndStatus() {
    ui->comboLevel->addItem(QStringLiteral("全部"), QVariant(-1));
    ui->comboLevel->addItem(QStringLiteral("Info"), QVariant(0));
    ui->comboLevel->addItem(QStringLiteral("Warning"), QVariant(1));
    ui->comboLevel->addItem(QStringLiteral("Critical"), QVariant(2));
    ui->comboStatus->addItem(QStringLiteral("全部"), QVariant(-1));
    ui->comboStatus->addItem(QStringLiteral("活跃"), QVariant(0));
    ui->comboStatus->addItem(QStringLiteral("已确认"), QVariant(1));
    ui->comboStatus->addItem(QStringLiteral("已恢复"), QVariant(2));
}

void AlarmCenterWidget::fillPoints(const std::shared_ptr<ens::protocol::PointTable>& pt) {
    ui->comboPoint->blockSignals(true);
    ui->comboPoint->clear();
    ui->comboPoint->addItem(QStringLiteral("全部"), QVariant(0));
    if (pt) {
        const auto points = pt->allPoints();
        for (const ens::protocol::PointRuntime* p : points) {
            ui->comboPoint->addItem(
                QStringLiteral("%1  %2").arg(p->pointId).arg(QString::fromStdString(p->pointName)),
                QVariant(static_cast<qlonglong>(p->pointId)));
        }
    }
    ui->comboPoint->blockSignals(false);
}

ens::datahub::AlarmQueryFilter AlarmCenterWidget::currentFilter() const {
    ens::datahub::AlarmQueryFilter f;
    f.level   = ui->comboLevel->currentData().toInt();
    f.status  = ui->comboStatus->currentData().toInt();
    f.pointId = static_cast<uint32_t>(ui->comboPoint->currentData().toLongLong());
    const uint64_t rangeMs = static_cast<uint64_t>(ui->comboRange->currentData().toLongLong());
    if (rangeMs > 0) {
        const uint64_t now = static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
        f.beginMs = now - rangeMs;
    }
    f.limit = 1000;   // UI 展示上限（FR-AL-11 万条内 <1s；千条展示足够）
    return f;
}

void AlarmCenterWidget::onQueryClicked() {
    if (!m_access) return;
    m_lastRows = m_access->queryAlarms(currentFilter());
    applyRows(m_lastRows);
    ui->btnExport->setEnabled(!m_lastRows.empty());
    ui->lblHint->setText(QStringLiteral("查询完成：%1 条告警").arg(m_lastRows.size()));
}

void AlarmCenterWidget::applyRows(const std::vector<ens::datahub::AlarmRecord>& rows) {
    m_model->setRowCount(0);
    for (const auto& r : rows) {
        const int row = m_model->rowCount();
        m_model->setItem(row, 0, new QStandardItem(fmtTime(r.triggerTime)));
        auto* lvl = new QStandardItem(levelText(r.level));
        lvl->setBackground(levelColor(r.level));
        m_model->setItem(row, 1, lvl);
        m_model->setItem(row, 2, new QStandardItem(statusText(r.status)));
        m_model->setItem(row, 3, new QStandardItem(QString::number(r.pointId)));
        QString name;
        if (m_pt) {
            const auto* p = m_pt->pointIdOf(r.pointId);
            if (p) name = QString::fromStdString(p->pointName);
        }
        m_model->setItem(row, 4, name.isEmpty() ? new QStandardItem(QStringLiteral("-"))
                                                : new QStandardItem(name));
        m_model->setItem(row, 5, new QStandardItem(fmtValue(r.alarmValue)));
        m_model->setItem(row, 6, new QStandardItem(fmtValue(r.threshold)));
        m_model->setItem(row, 7, r.confirmUser.isEmpty() ? new QStandardItem(QStringLiteral("-"))
                                                         : new QStandardItem(r.confirmUser));
        m_model->setItem(row, 8, new QStandardItem(fmtTime(r.confirmTime)));
        m_model->setItem(row, 9, new QStandardItem(fmtTime(r.recoverTime)));
        m_model->setItem(row, 10, new QStandardItem(r.description));
        m_model->item(row, 0)->setData(static_cast<qlonglong>(r.id), kIdRole);
    }
}

uint64_t AlarmCenterWidget::alarmIdOf(const QStandardItemModel* model, int row) {
    const QStandardItem* it = model->item(row, 0);
    return it ? static_cast<uint64_t>(it->data(kIdRole).toLongLong()) : 0;
}

void AlarmCenterWidget::onAlarmTriggered(const ens::business::AlarmEvent&) {
    // 新告警 → 1s 防抖重查（风暴路径不 emit alarmTriggered，天然防刷屏）
    m_debounceTimer.start();
}

void AlarmCenterWidget::onRefreshActive() {
    if (m_alarm) {
        ui->lblActive->setText(QString::number(m_alarm->activeAlarmCount()));
    }
}

void AlarmCenterWidget::onConfirmClicked() {
    if (!m_alarm || !m_currentUser) return;
    const QString user = m_currentUser();
    if (user.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("确认告警"),
                                 QStringLiteral("当前无登录用户，无法记录确认人"));
        return;
    }
    const QModelIndexList sel = ui->tableAlarms->selectionModel()->selectedRows(0);
    std::vector<uint64_t> ids;
    for (const QModelIndex& idx : sel) {
        const uint64_t id = alarmIdOf(m_model, idx.row());
        if (id != 0) ids.push_back(id);
    }
    if (ids.empty()) {
        QMessageBox::information(this, QStringLiteral("确认告警"),
                                 QStringLiteral("请先选择要确认的告警行"));
        return;
    }
    m_alarm->acknowledgeAlarms(ids, user);
    onQueryClicked();   // 刷新状态列
}

void AlarmCenterWidget::onExportClicked() {
    if (m_lastRows.empty()) return;
    const QString defaultName = QStringLiteral("alarms_%1.csv").arg(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出告警历史 CSV"), defaultName, QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty()) return;

    ens::ui::CsvWriter writer(std::filesystem::path(path.toStdWString()));
    if (!writer.open()) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("无法创建文件：\n%1").arg(path));
        return;
    }
    // FR-EXP-03：含 FR-AL-13 全部审计字段
    writer.writeRow({"触发时间", "级别", "状态", "测点ID", "测点名称", "告警值", "阈值",
                     "确认人", "确认时间", "恢复时间", "描述"});
    for (const auto& r : m_lastRows) {
        QString name;
        if (m_pt) {
            const auto* p = m_pt->pointIdOf(r.pointId);
            if (p) name = QString::fromStdString(p->pointName);
        }
        writer.writeRow({fmtTime(r.triggerTime).toStdString(), levelText(r.level).toStdString(),
                         statusText(r.status).toStdString(), std::to_string(r.pointId),
                         name.toStdString(), fmtValue(r.alarmValue).toStdString(),
                         fmtValue(r.threshold).toStdString(), r.confirmUser.toStdString(),
                         fmtTime(r.confirmTime).toStdString(), fmtTime(r.recoverTime).toStdString(),
                         r.description.toStdString()});
    }
    writer.close();
    ui->lblHint->setText(QStringLiteral("已导出 %1 条告警 → %2")
                             .arg(m_lastRows.size()).arg(QDir::toNativeSeparators(path)));
}

}  // namespace ens::ui
