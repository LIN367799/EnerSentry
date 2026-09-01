// src/gui/log_view.cpp —— B10 LogView 实现（ENS-SIM-IMP §10.1/§10.3）
// sim_events.jsonl 每行 JSON：{"ts":ms,"level":"INFO|FAULT|RECOVER","event":"..."}
// 增量策略：轮询取全量字符串 → 按行切分 → 与本地 m_lines 比对追加（事件低频，1s 足够）。
#include "gui/log_view.h"
#include "ui_log_view.h"

#include "sim/SimulatorEngine.h"

#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QTextStream>

#include <algorithm>

LogView::LogView(ens::sim::SimulatorEngine* engine, QWidget* parent)
    : QWidget(parent), ui(new Ui::LogView), m_engine(engine) {
    ui->setupUi(this);
    connect(&m_timer, &QTimer::timeout, this, &LogView::onPollTick);
    connect(ui->comboLevel, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &LogView::onLevelFilterChanged);
    connect(ui->btnClear, &QPushButton::clicked, this, &LogView::onClearClicked);
    connect(ui->btnExport, &QPushButton::clicked, this, &LogView::onExportClicked);
}

LogView::~LogView() {
    m_timer.stop();
    delete ui;
}

void LogView::setEngineRunning(bool running) {
    m_engineRunning = running;
    if (running) {
        m_lastCount = 0;
        m_timer.start(1000);
    } else {
        m_timer.stop();
    }
}

void LogView::onPollTick() {
    if (!m_engine || !m_engineRunning) return;
    const std::string jsonl = m_engine->scenarioEventsJsonl();
    if (jsonl.empty()) return;

    const QStringList lines =
        QString::fromStdString(jsonl).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    const int total = lines.size();
    if (total <= m_lastCount) return;  // 无新增
    for (int i = m_lastCount; i < total; ++i) {
        m_lines.append(lines[i]);
    }
    m_lastCount = total;
    repaintFiltered();
}

void LogView::onLevelFilterChanged() {
    repaintFiltered();
}

void LogView::repaintFiltered() {
    const int filter = ui->comboLevel->currentIndex();
    // 0 全部 / 1 故障 FAULT / 2 恢复 RECOVER / 3 信息 INFO
    auto matches = [filter](const QString& line) {
        switch (filter) {
            case 1: return line.contains(QStringLiteral("FAULT"), Qt::CaseInsensitive);
            case 2: return line.contains(QStringLiteral("RECOVER"), Qt::CaseInsensitive);
            case 3: return line.contains(QStringLiteral("\"INFO\""), Qt::CaseInsensitive) ||
                           !(line.contains(QStringLiteral("FAULT")) ||
                             line.contains(QStringLiteral("RECOVER")));
            default: return true;
        }
    };

    ui->textLog->clear();
    QTextCursor cur(ui->textLog->document());
    for (const QString& line : m_lines) {
        if (!matches(line)) continue;
        QTextCharFormat fmt;
        const QString fg = line.contains(QStringLiteral("FAULT")) ? QStringLiteral("#e24b4a")
                         : line.contains(QStringLiteral("RECOVER")) ? QStringLiteral("#97c459")
                         : QStringLiteral("#85b7eb");
        fmt.setForeground(QColor(fg));
        cur.insertText(line + QLatin1Char('\n'), fmt);
    }
    // 滚动到底部
    QScrollBar* sb = ui->textLog->verticalScrollBar();
    if (sb) sb->setValue(sb->maximum());
}

void LogView::onClearClicked() {
    m_lines.clear();
    m_lastCount = 0;
    ui->textLog->clear();
}

void LogView::onExportClicked() {
    if (m_lines.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("导出"), QStringLiteral("暂无日志可导出"));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出日志"), QStringLiteral("sim_events.txt"),
        QStringLiteral("文本文件 (*.txt)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("导出"), QStringLiteral("无法写入：%1").arg(path));
        return;
    }
    QTextStream ts(&f);
    ts.setCodec("UTF-8");  // Qt 5.15（QStringConverter 为 Qt 6 API，不用）
    for (const QString& l : m_lines) ts << l << QLatin1Char('\n');
    f.close();
}
