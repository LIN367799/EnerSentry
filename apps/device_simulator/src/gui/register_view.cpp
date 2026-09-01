// src/gui/register_view.cpp —— B10 RegisterView 实现（ENS-SIM-IMP §10.1）
// 30Hz 轮询 RegisterBank::snapshot() 填 QStandardItemModel。
// 取值：HoldingRegister→getHolding / InputRegister→getInput / Coil→getCoil / Discrete→getDiscrete。
// 工程值 = raw * scaleFactor + offset（线性换算；Float32 多寄存器字节重组超出本视图范围，
//         B5 物理演化器已把工程值量化进 raw，线性换算近似足够演示与验收）。
// 告警列：SlaveRegset::alarmWord bit0=过温 bit1=过压 bit2=欠压（B8 本地定义，联调与主程序对齐）。
#include "gui/register_view.h"
#include "ui_register_view.h"

#include "core/point_table.h"
#include "sim/SimulatorEngine.h"
#include "sim/register_bank.h"

#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>

namespace {
const char* regTypeName(ens::sim::RegisterType t) {
    switch (t) {
        case ens::sim::RegisterType::HoldingRegister: return "Holding";
        case ens::sim::RegisterType::InputRegister:   return "Input";
        case ens::sim::RegisterType::Coil:            return "Coil";
        case ens::sim::RegisterType::DiscreteInput:   return "Discrete";
    }
    return "?";
}

// alarmWord bit 定义（B8/register_bank.h）：bit0=OverTemp bit1=OverVoltage bit2=UnderVoltage
QString alarmText(uint16_t word) {
    QStringList parts;
    if (word & 0x01u) parts << QStringLiteral("过温");
    if (word & 0x02u) parts << QStringLiteral("过压");
    if (word & 0x04u) parts << QStringLiteral("欠压");
    return parts.join(u'|');
}
}  // namespace

RegisterView::RegisterView(ens::sim::SimulatorEngine* engine, QWidget* parent)
    : QWidget(parent), ui(new Ui::RegisterView), m_engine(engine) {
    ui->setupUi(this);

    m_treeModel  = new QStandardItemModel(this);
    m_tableModel = new QStandardItemModel(this);
    ui->treeSlaves->setModel(m_treeModel);
    ui->tableRegs->setModel(m_tableModel);

    m_tableModel->setHorizontalHeaderLabels(
        {QStringLiteral("点名"), QStringLiteral("类型"), QStringLiteral("地址"),
         QStringLiteral("原始(hex)"), QStringLiteral("原始(dec)"), QStringLiteral("工程值"),
         QStringLiteral("单位"), QStringLiteral("告警")});
    ui->tableRegs->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->tableRegs->verticalHeader()->setVisible(false);

    rebuildTree();
    rebuildPointRows();

    m_timer.setInterval(33);  // ~30Hz（ADR-22 刷新纪律）
    connect(&m_timer, &QTimer::timeout, this, &RegisterView::onRefreshTick);
    m_timer.start();

    connect(ui->editFilter, &QLineEdit::textChanged, this, &RegisterView::onNameFilterChanged);
    connect(ui->comboAlarm, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &RegisterView::onAlarmFilterChanged);
    connect(ui->treeSlaves, &QTreeView::clicked, this, &RegisterView::onTreeClicked);
}

RegisterView::~RegisterView() {
    m_timer.stop();
    delete ui;
}

void RegisterView::setEngineRunning(bool running) {
    m_engineRunning = running;
    if (!running) {
        // 引擎停止 → 表置空，避免残留旧值误导
        m_tableModel->removeRows(0, m_tableModel->rowCount());
    }
}

// ── 设备树：slave 节点（名称按 kind 标注）+ 该从站全部点 ──
void RegisterView::rebuildTree() {
    m_treeModel->clear();
    m_slaveTreeRow.clear();
    if (!m_engine) return;
    const ens::sim::SimPointTable* pt = m_engine->pointTable();
    if (!pt) return;

    auto slaves = pt->allSlaveIds();   // 注意：需可变容器（下面 std::sort）
    std::sort(slaves.begin(), slaves.end());
    for (uint8_t s : slaves) {
        const auto& pts = pt->onSlave(s);
        auto* node = new QStandardItem(QStringLiteral("从站 %1  (%2 点)").arg(s).arg(pts.size()));
        node->setEditable(false);
        m_treeModel->appendRow(node);
        m_slaveTreeRow.insert(s, m_treeModel->rowCount() - 1);
        for (const auto& p : pts) {
            auto* child = new QStandardItem(QString::fromStdString(p.pointName));
            child->setEditable(false);
            child->setData(QVariant::fromValue(uint32_t{s}), Qt::UserRole);  // 行内标记所属 slave
            node->appendRow(child);
        }
    }
    ui->treeSlaves->expandAll();
}

// ── 表行重建：应用 从站/名称/告警 过滤 ──
void RegisterView::rebuildPointRows() {
    m_points.clear();
    m_pointSlaves.clear();
    if (!m_engine) return;
    const ens::sim::SimPointTable* pt = m_engine->pointTable();
    if (!pt) return;

    auto slaves = pt->allSlaveIds();   // 需可变容器（std::sort 就地排序）
    const QString nameFilter = ui->editFilter->text().trimmed();
    const bool alarmOnly = (ui->comboAlarm->currentIndex() == 1);

    for (uint8_t s : slaves) {
        if (m_slaveFilter != 0 && m_slaveFilter != s) continue;
        for (const auto& p : pt->onSlave(s)) {
            if (!nameFilter.isEmpty() &&
                !QString::fromStdString(p.pointName).contains(nameFilter, Qt::CaseInsensitive)) {
                continue;
            }
            m_points.push_back(&p);
            m_pointSlaves.push_back(s);
        }
    }
    (void)alarmOnly;  // 告警过滤在 refreshTable 内按快照 alarmWord 判定
    m_tableModel->setRowCount(static_cast<int>(m_points.size()));
    refreshTable();
}

// ── 30Hz 刷新：按 slave 取一次快照，填该从站全部点行 ──
void RegisterView::onRefreshTick() {
    if (!m_engine || !m_engineRunning || m_refreshing) return;
    ens::sim::RegisterBank* bank = m_engine->bank();
    if (!bank) return;
    refreshTable();
}

void RegisterView::refreshTable() {
    if (m_points.isEmpty()) return;
    ens::sim::RegisterBank* bank = m_engine->bank();
    if (!bank) return;

    const bool alarmOnly = (ui->comboAlarm->currentIndex() == 1);
    int visibleRow = 0;
    uint8_t curSlave = 0;
    std::shared_ptr<const ens::sim::SlaveRegset> snap;

    for (int i = 0; i < m_points.size(); ++i) {
        const ens::sim::SimPoint* p = m_points[i];
        const uint8_t s = m_pointSlaves[i];
        if (s != curSlave) {  // 同从站共享快照（RCU 拷贝 shared_ptr，无锁读）
            curSlave = s;
            snap = bank->snapshot(s);
        }
        const ens::sim::SlaveRegset* regs = snap.get();

        uint16_t raw = 0;
        bool ok = false;
        if (regs) {
            switch (p->regType) {
                case ens::sim::RegisterType::HoldingRegister:
                    raw = regs->getHolding(p->registerAddr); ok = true; break;
                case ens::sim::RegisterType::InputRegister:
                    raw = regs->getInput(p->registerAddr);   ok = true; break;
                case ens::sim::RegisterType::Coil:
                    raw = regs->getCoil(p->registerAddr) ? 1 : 0; ok = true; break;
                case ens::sim::RegisterType::DiscreteInput:
                    raw = regs->getDiscrete(p->registerAddr) ? 1 : 0; ok = true; break;
            }
        }
        if (!ok) raw = 0;

        const QString alarm = regs ? alarmText(regs->alarmWord) : QString();
        if (alarmOnly && alarm.isEmpty()) continue;  // 仅告警过滤：非告警行跳过

        const float eng = raw * p->scaleFactor + p->offset;
        const int row = visibleRow;
        m_tableModel->setItem(row, 0, new QStandardItem(QString::fromStdString(p->pointName)));
        m_tableModel->setItem(row, 1, new QStandardItem(QString::fromLatin1(regTypeName(p->regType))));
        m_tableModel->setItem(row, 2, new QStandardItem(QString::number(p->registerAddr)));
        m_tableModel->setItem(row, 3, new QStandardItem(QStringLiteral("0x%1").arg(raw, 4, 16, QLatin1Char('0'))));
        m_tableModel->setItem(row, 4, new QStandardItem(QString::number(raw)));
        m_tableModel->setItem(row, 5, new QStandardItem(QString::number(static_cast<double>(eng), 'f', 2)));
        m_tableModel->setItem(row, 6, new QStandardItem(QString::fromStdString(p->unit)));
        m_tableModel->setItem(row, 7, new QStandardItem(alarm));
        if (!alarm.isEmpty()) {
            // 告警行背景浅红（临时色；5A QSS 主题统一后改由样式表控制）
            for (int c = 0; c < 8; ++c) {
                m_tableModel->item(row, c)->setBackground(QColor(0x4a, 0x1a, 0x1a));
            }
        }
        ++visibleRow;
    }
    m_tableModel->setRowCount(visibleRow);
}

void RegisterView::onNameFilterChanged() {
    if (m_refreshing) return;
    m_refreshing = true;
    rebuildPointRows();
    m_refreshing = false;
}

void RegisterView::onAlarmFilterChanged() {
    if (m_refreshing) return;
    m_refreshing = true;
    rebuildPointRows();
    m_refreshing = false;
}

void RegisterView::onTreeClicked(const QModelIndex& idx) {
    if (!idx.isValid()) return;
    // 树节点：slave 行 → 过滤该从站；点行 → 过滤该从站（表内可见）
    const QStandardItem* item = m_treeModel->itemFromIndex(idx);
    if (!item) return;
    // 通过行的第一列 item 的 UserRole 拿 slaveId（子节点带；父节点无则按树行号反查）
    bool hasSlave = item->data(Qt::UserRole).isValid();
    uint8_t s = hasSlave ? static_cast<uint8_t>(item->data(Qt::UserRole).toUInt()) : 0;
    if (!hasSlave) {
        // 父节点：树行号 → slaveId（反查 m_slaveTreeRow）
        for (auto it = m_slaveTreeRow.cbegin(); it != m_slaveTreeRow.cend(); ++it) {
            if (it.value() == idx.row()) { s = it.key(); break; }
        }
    }
    m_slaveFilter = s;
    m_refreshing = true;
    rebuildPointRows();
    m_refreshing = false;
}
