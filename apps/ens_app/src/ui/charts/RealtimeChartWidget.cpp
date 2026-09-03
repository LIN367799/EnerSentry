// src/ui/charts/RealtimeChartWidget.cpp —— 实时曲线容器实现（切片 20/39）。
#include "charts/RealtimeChartWidget.h"

#include "charts/RealtimePlotWidget.h"

#include <QDateTime>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QPushButton>
#include <QVBoxLayout>

namespace ens::ui {

namespace {
constexpr int kRowPointRole = Qt::UserRole;   // 列表行 → pointId
}

void RealtimeChartSubscriber::onSample(const ens::datahub::Sample& s) noexcept {
    // 任意线程回调：只转发（线程安全经 QueuedConnection）
    m_w->onSampleBridge(s);
}

RealtimeChartWidget::RealtimeChartWidget(ens::datahub::DataBus* bus, QWidget* parent)
    : QWidget(parent), m_bus(bus), m_plot(new RealtimePlotWidget(this)),
      m_sub(this) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    // ── 顶栏：标尺开关 + 提示（切片 39）──
    auto* top = new QHBoxLayout();
    m_btnRuler = new QPushButton(QStringLiteral("标尺"), this);
    m_btnRuler->setCheckable(true);
    m_btnRuler->setToolTip(QStringLiteral("FR-RT-07：垂直标尺，拖动读取全部通道数值"));
    top->addWidget(m_btnRuler);
    auto* pngBtn = new QPushButton(QStringLiteral("截图 PNG"), this);
    pngBtn->setToolTip(QStringLiteral("FR-EXP-02：导出曲线截图"));
    top->addWidget(pngBtn);
    auto* hint = new QLabel(QStringLiteral(
        "实时曲线（30Hz 批处理 + min/max 降采样；悬停读值；列表勾选分配右轴）"), this);
    hint->setStyleSheet(QStringLiteral("color: #8b949e; padding: 2px 6px;"));
    top->addWidget(hint, 1);
    root->addLayout(top);

    // ── 主体：通道列表（checkbox 右轴）+ 曲线 ──
    auto* body = new QHBoxLayout();
    m_chList = new QListWidget(this);
    m_chList->setFixedWidth(190);
    m_chList->setStyleSheet(QStringLiteral(
        "QListWidget { background: #14181d; border: 1px solid #343b47; color: #d8dee6; }"));
    body->addWidget(m_chList);
    body->addWidget(m_plot, 1);
    root->addLayout(body, 1);

    connect(m_btnRuler, &QPushButton::toggled, this, &RealtimeChartWidget::onRulerToggled);
    connect(pngBtn, &QPushButton::clicked, this, &RealtimeChartWidget::onPngClicked);
    connect(m_plot, &RealtimePlotWidget::channelAdded,
            this, &RealtimeChartWidget::onChannelAdded);
    connect(m_chList, &QListWidget::itemChanged,
            this, &RealtimeChartWidget::onChannelItemChanged);

    if (m_bus) {
        m_handle = m_bus->subscribeWildcard(&m_sub);
    }
}

RealtimeChartWidget::~RealtimeChartWidget() {
    if (m_bus && m_handle != 0) {
        m_bus->unsubscribe(m_handle);
    }
}

bool RealtimeChartWidget::rulerToggled() const {
    return m_btnRuler ? m_btnRuler->isChecked() : false;
}

int RealtimeChartWidget::channelListCount() const {
    return m_chList ? m_chList->count() : 0;
}

void RealtimeChartWidget::onSampleBridge(const ens::datahub::Sample& s) {
    // 主线程投递（跨线程安全；样本高频时按 pointId 建通道一次后只转发）
    QMetaObject::invokeMethod(m_plot, [this, pid = s.pointId, v = double(s.value), ts = s.timestamp]() {
        if (!m_plot->hasChannel(pid) && m_plot->channelCount() < kMaxChannels) {
            m_plot->addChannel(pid, QStringLiteral("pt %1").arg(pid), QColor());
        }
        m_plot->onNewSample(pid, v, static_cast<qint64>(ts));
    }, Qt::QueuedConnection);
}

void RealtimeChartWidget::addListRow(uint32_t pointId, const QString& name) {
    auto* item = new QListWidgetItem(name, m_chList);
    item->setData(kRowPointRole, static_cast<qlonglong>(pointId));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);   // 默认左轴
    m_chList->addItem(item);
}

void RealtimeChartWidget::onChannelAdded(uint32_t pointId, const QString& name) {
    // 避免重复行（订阅重建/clearAll 后）
    for (int i = 0; i < m_chList->count(); ++i) {
        if (m_chList->item(i)->data(kRowPointRole).toLongLong() == pointId) return;
    }
    addListRow(pointId, name);
}

void RealtimeChartWidget::onChannelItemChanged(QListWidgetItem* item) {
    const uint32_t pid = static_cast<uint32_t>(item->data(kRowPointRole).toLongLong());
    if (pid == 0) return;
    const bool right = (item->checkState() == Qt::Checked);
    m_plot->setChannelAxis(pid, right ? RealtimePlotWidget::AxisSide::Right
                                      : RealtimePlotWidget::AxisSide::Left);
}

void RealtimeChartWidget::onRulerToggled(bool checked) {
    m_plot->setRulerEnabled(checked);
}

void RealtimeChartWidget::onPngClicked() {
    // 切片 41：FR-EXP-02 实时曲线截图
    if (m_plot->channelCount() == 0) return;
    const QString defaultName = QStringLiteral("realtime_%1.png").arg(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出实时曲线 PNG"), defaultName,
        QStringLiteral("PNG 图片 (*.png)"));
    if (path.isEmpty()) return;
    m_plot->savePng(path);
}

}  // namespace ens::ui
