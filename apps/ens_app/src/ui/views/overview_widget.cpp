// src/ui/views/overview_widget.cpp —— 总览实现（切片 19/40）。
#include "views/overview_widget.h"
#include "ui_OverviewWidget.h"

#include "common/OverviewPoints.h"
#include "controls/SocGauge.h"
#include "controls/TempHeatBar.h"
#include "PointTable.h"

#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace ens::ui {

namespace {
constexpr int kDrillLevelRole = Qt::UserRole;
constexpr int kDrillRackRole = Qt::UserRole + 1;
constexpr int kDrillPointRole = Qt::UserRole + 2;

enum class DrillLevel : int { Station = 0, Rack = 1, Point = 2 };

QString joinParts(const QStringList& parts) {
    return parts.isEmpty() ? QStringLiteral("--") : parts.join(QStringLiteral(" / "));
}
}  // namespace

OverviewWidget::OverviewWidget(ens::datahub::DataBus* bus,
                               const std::shared_ptr<ens::protocol::PointTable>& pt,
                               QWidget* parent)
    : QWidget(parent), ui(new Ui::OverviewWidget), m_bus(bus), m_pt(pt) {
    ui->setupUi(this);

    // ── 切片 40：SOC 仪表盘 + 簇最高温热力条 ──
    m_gauge = new SocGauge(this);
    ui->socLayout->addWidget(m_gauge);
    m_heat = new TempHeatBar(this);
    ui->heatLayout->addWidget(m_heat);

    buildPointIndex();
    setupDrilldownUi();
    populateDrillTree();
    if (m_pt && m_socIds.isEmpty() && m_maxTempByRack.isEmpty()) {
        // 点表存在但无 Rack-* 约定点：保留禁用态（控件空）
    }

    if (m_bus) {
        m_handle = m_bus->subscribeWildcard(&m_sub);
    }
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &OverviewWidget::onRefreshUi);
    m_timer.start();
}

OverviewWidget::~OverviewWidget() {
    m_timer.stop();
    if (m_bus && m_handle != 0) {
        m_bus->unsubscribe(m_handle);
    }
    delete ui;
}

QString OverviewWidget::drillBreadcrumb() const {
    return m_drillBreadcrumb ? m_drillBreadcrumb->text() : QString();
}

void OverviewWidget::buildPointIndex() {
    if (!m_pt) return;
    m_socIds.clear();
    m_socByRack.clear();
    m_maxTempByRack.clear();
    m_rackOrder.clear();
    m_pointsByRack.clear();
    m_pointInfo.clear();
    for (const auto* p : m_pt->allPoints()) {
        if (!p->enabled) continue;
        const QString name = QString::fromStdString(p->pointName);
        const int rackNo = rackNoFromName(name);
        if (rackNo > 0) {
            OverviewDrillPoint dp;
            dp.name = name;
            dp.unit = QString::fromStdString(p->unit);
            dp.rackNo = rackNo;
            dp.slave = p->slaveAddress;
            dp.registerAddr = p->registerAddr;
            m_pointInfo.insert(p->pointId, dp);
            m_pointsByRack[rackNo].push_back(p->pointId);
        }

        const OvrPointInfo info = ovrClassifyName(name);
        if (info.kind == OvrKind::Soc) {
            m_socIds.push_back(p->pointId);
            if (info.rackNo > 0) {
                m_socByRack.insert(info.rackNo, p->pointId);
            }
        } else if (info.kind == OvrKind::ClusterMaxTemp && info.rackNo > 0) {
            if (!m_maxTempByRack.contains(info.rackNo)) {
                m_maxTempByRack.insert(info.rackNo, p->pointId);
            }
        }
    }
    m_rackOrder = m_pointsByRack.keys().toVector();   // QList<int> → QVector<int>
    std::sort(m_rackOrder.begin(), m_rackOrder.end());
    for (int rackNo : m_rackOrder) {
        auto& ids = m_pointsByRack[rackNo];
        std::sort(ids.begin(), ids.end(), [this](uint32_t lhs, uint32_t rhs) {
            const auto li = m_pointInfo.constFind(lhs);
            const auto ri = m_pointInfo.constFind(rhs);
            const uint16_t la = (li == m_pointInfo.constEnd()) ? 0 : li->registerAddr;
            const uint16_t ra = (ri == m_pointInfo.constEnd()) ? 0 : ri->registerAddr;
            if (la != ra) return la < ra;
            return lhs < rhs;
        });
    }
    m_drillRackCount = m_rackOrder.size();
}

void OverviewWidget::setupDrilldownUi() {
    auto* group = new QGroupBox(QStringLiteral("拓扑钻取"), this);
    group->setObjectName(QStringLiteral("grpDrilldown"));
    auto* lay = new QVBoxLayout(group);
    lay->setContentsMargins(8, 8, 8, 8);

    m_drillBreadcrumb = new QLabel(QStringLiteral("电站"), group);
    m_drillBreadcrumb->setObjectName(QStringLiteral("drillBreadcrumb"));
    m_drillBreadcrumb->setStyleSheet(QStringLiteral("color: #e0c060; font-weight: 600;"));
    lay->addWidget(m_drillBreadcrumb);

    m_drillTree = new QTreeWidget(group);
    m_drillTree->setObjectName(QStringLiteral("drillTree"));
    m_drillTree->setColumnCount(2);
    m_drillTree->setHeaderLabels({QStringLiteral("层级 / 测点"), QStringLiteral("实时值")});
    m_drillTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_drillTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_drillTree->setMinimumWidth(360);
    m_drillTree->setMinimumHeight(190);
    lay->addWidget(m_drillTree, 1);

    m_drillSummary = new QLabel(group);
    m_drillSummary->setObjectName(QStringLiteral("drillSummary"));
    m_drillSummary->setWordWrap(true);
    m_drillSummary->setStyleSheet(QStringLiteral("color: #9fb3c8;"));
    lay->addWidget(m_drillSummary);

    ui->rowTop->insertWidget(2, group, 1);

    connect(m_drillTree, &QTreeWidget::currentItemChanged,
            this, &OverviewWidget::onDrillItemChanged);
    connect(m_drillTree, &QTreeWidget::itemActivated,
            this, &OverviewWidget::onDrillItemActivated);
}

void OverviewWidget::populateDrillTree() {
    if (!m_drillTree) return;
    m_drillTree->clear();

    auto* station = new QTreeWidgetItem(m_drillTree,
        {QStringLiteral("储能电站"), QStringLiteral("--")});
    station->setData(0, kDrillLevelRole, static_cast<int>(DrillLevel::Station));
    station->setExpanded(true);

    for (int rackNo : m_rackOrder) {
        auto* rackItem = new QTreeWidgetItem(station,
            {ovrRackLabel(rackNo), QStringLiteral("--")});
        rackItem->setData(0, kDrillLevelRole, static_cast<int>(DrillLevel::Rack));
        rackItem->setData(0, kDrillRackRole, rackNo);

        const auto ids = m_pointsByRack.value(rackNo);
        for (uint32_t pid : ids) {
            const auto it = m_pointInfo.constFind(pid);
            if (it == m_pointInfo.constEnd()) continue;
            auto* pointItem = new QTreeWidgetItem(rackItem,
                {it->name, QStringLiteral("--")});
            pointItem->setData(0, kDrillLevelRole, static_cast<int>(DrillLevel::Point));
            pointItem->setData(0, kDrillRackRole, rackNo);
            pointItem->setData(0, kDrillPointRole, static_cast<qlonglong>(pid));
        }
    }

    m_drillTree->setCurrentItem(station);
    refreshDrillTreeValues();
    updateDrillSummary(station);
}

void OverviewWidget::onRefreshUi() {
    const uint64_t count = m_sub.count();
    ui->lblCount->setText(QString::number(count));
    if (count > 0) {
        ui->lblLastPid->setText(QString::number(m_sub.lastPid()));
        ui->lblLastVal->setText(QString::number(static_cast<double>(m_sub.lastVal()), 'f', 2));
        static uint64_t s_prev = 0;
        const uint64_t delta = count - s_prev;
        s_prev = count;
        ui->lblRate->setText(QString::number(delta * 2));  // 500ms 采样 × 2 = 点/秒
    }

    // ── FR-OV-06：整站 SOC = 活跃 Rack SOC 算术平均 ──
    if (!m_socIds.isEmpty()) {
        int n = 0;
        double sum = 0.0;
        for (uint32_t pid : m_socIds) {
            float v = 0.0f;
            if (m_sub.cached(pid, &v)) {
                sum += qBound(0.0, static_cast<double>(v), 100.0);
                ++n;
            }
        }
        if (n > 0) {
            m_lastSoc = sum / n;
            m_gauge->setValue(m_lastSoc);
        }
    }

    // ── FR-OV-07：各簇最高温热力条 ──
    QVector<TempHeatBar::Cell> cells;
    cells.reserve(m_rackOrder.size());
    for (int rackNo : m_rackOrder) {
        TempHeatBar::Cell c;
        c.label = ovrRackLabel(rackNo);
        const auto it = m_maxTempByRack.constFind(rackNo);
        float v = 0.0f;
        if (it != m_maxTempByRack.constEnd() && m_sub.cached(it.value(), &v)) {
            c.temp = v;
            c.valid = true;
        }
        cells.push_back(c);
    }
    m_heatCount = cells.size();
    m_heat->setCells(cells);
    refreshDrillTreeValues();
    updateDrillSummary(m_drillTree ? m_drillTree->currentItem() : nullptr);
}

void OverviewWidget::onDrillItemChanged(QTreeWidgetItem* current,
                                        QTreeWidgetItem* previous) {
    Q_UNUSED(previous);
    updateDrillSummary(current);
}

void OverviewWidget::onDrillItemActivated(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    if (!item) return;
    if (item->childCount() > 0) {
        item->setExpanded(!item->isExpanded());
    }
    updateDrillSummary(item);
}

void OverviewWidget::refreshDrillTreeValues() {
    if (!m_drillTree || m_drillTree->topLevelItemCount() == 0) return;
    auto* station = m_drillTree->topLevelItem(0);
    station->setText(1, QStringLiteral("%1 samples").arg(m_sub.count()));

    for (int ri = 0; ri < station->childCount(); ++ri) {
        auto* rackItem = station->child(ri);
        const int rackNo = rackItem->data(0, kDrillRackRole).toInt();
        QStringList rackParts;
        float soc = 0.0f;
        const auto socIt = m_socByRack.constFind(rackNo);
        if (socIt != m_socByRack.constEnd() && m_sub.cached(socIt.value(), &soc)) {
            rackParts << QStringLiteral("SOC %1%").arg(soc, 0, 'f', 1);
        }
        float temp = 0.0f;
        const auto tempIt = m_maxTempByRack.constFind(rackNo);
        if (tempIt != m_maxTempByRack.constEnd() && m_sub.cached(tempIt.value(), &temp)) {
            rackParts << QStringLiteral("%1 ℃").arg(temp, 0, 'f', 1);
        }
        rackItem->setText(1, joinParts(rackParts));

        for (int pi = 0; pi < rackItem->childCount(); ++pi) {
            auto* pointItem = rackItem->child(pi);
            const uint32_t pid =
                static_cast<uint32_t>(pointItem->data(0, kDrillPointRole).toLongLong());
            const auto info = m_pointInfo.constFind(pid);
            pointItem->setText(1, info == m_pointInfo.constEnd()
                                      ? QStringLiteral("--")
                                      : formatPointValue(pid, info->unit));
        }
    }
}

void OverviewWidget::updateDrillSummary(QTreeWidgetItem* item) {
    if (!m_drillBreadcrumb || !m_drillSummary) return;
    if (!item) {
        m_drillBreadcrumb->setText(QStringLiteral("电站"));
        m_drillSummary->setText(QStringLiteral("暂无拓扑数据"));
        return;
    }

    const auto level = static_cast<DrillLevel>(item->data(0, kDrillLevelRole).toInt());
    if (level == DrillLevel::Station) {
        m_drillBreadcrumb->setText(QStringLiteral("电站"));
        m_drillSummary->setText(QStringLiteral("Rack %1 个；整站 SOC %2%；累计样本 %3")
            .arg(m_rackOrder.size())
            .arg(m_lastSoc, 0, 'f', 1)
            .arg(m_sub.count()));
        return;
    }

    const int rackNo = item->data(0, kDrillRackRole).toInt();
    if (level == DrillLevel::Rack) {
        QStringList parts;
        float soc = 0.0f;
        const auto socIt = m_socByRack.constFind(rackNo);
        if (socIt != m_socByRack.constEnd() && m_sub.cached(socIt.value(), &soc)) {
            parts << QStringLiteral("SOC %1%").arg(soc, 0, 'f', 1);
        }
        float temp = 0.0f;
        const auto tempIt = m_maxTempByRack.constFind(rackNo);
        if (tempIt != m_maxTempByRack.constEnd() && m_sub.cached(tempIt.value(), &temp)) {
            parts << QStringLiteral("最高温 %1 ℃").arg(temp, 0, 'f', 1);
        }
        m_drillBreadcrumb->setText(QStringLiteral("电站 / %1").arg(ovrRackLabel(rackNo)));
        m_drillSummary->setText(QStringLiteral("%1；测点 %2 个")
            .arg(joinParts(parts))
            .arg(m_pointsByRack.value(rackNo).size()));
        return;
    }

    const uint32_t pid =
        static_cast<uint32_t>(item->data(0, kDrillPointRole).toLongLong());
    const auto info = m_pointInfo.constFind(pid);
    if (info == m_pointInfo.constEnd()) return;
    m_drillBreadcrumb->setText(QStringLiteral("电站 / %1 / %2")
        .arg(ovrRackLabel(info->rackNo), info->name));
    m_drillSummary->setText(QStringLiteral("pointId=%1；slave=%2；addr=%3；当前 %4")
        .arg(pid)
        .arg(info->slave)
        .arg(info->registerAddr)
        .arg(formatPointValue(pid, info->unit)));
}

QString OverviewWidget::formatPointValue(uint32_t pointId, const QString& unit) const {
    float value = 0.0f;
    if (!m_sub.cached(pointId, &value)) return QStringLiteral("--");
    return unit.isEmpty()
        ? QStringLiteral("%1").arg(value, 0, 'f', 2)
        : QStringLiteral("%1 %2").arg(value, 0, 'f', 2).arg(unit);
}

int OverviewWidget::rackNoFromName(const QString& name) {
    if (!name.startsWith(QStringLiteral("Rack-"))) return -1;
    int i = 5;
    int rackNo = 0;
    bool any = false;
    while (i < name.size() && name[i].isDigit()) {
        rackNo = rackNo * 10 + (name[i].unicode() - '0');
        ++i;
        any = true;
    }
    if (!any || i >= name.size() || name[i] != QLatin1Char('_')) return -1;
    return rackNo;
}

}  // namespace ens::ui
