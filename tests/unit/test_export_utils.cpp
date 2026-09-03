// tests/unit/test_export_utils.cpp —— 导出工具（切片 41，FR-EXP-02/05/06）。
// 覆盖：① copyDirRecursive 递归拷贝（子目录/文件/内容一致/源缺失失败）
//       ② copyFileToDir 单文件拷贝（覆盖已存在）
//       ③ RealtimePlotWidget::savePng 落盘（size>0，FR-EXP-02）

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "ui/charts/RealtimePlotWidget.h"
#include "ui/common/ExportUtils.h"

using ens::ui::copyDirRecursive;
using ens::ui::copyFileToDir;
using ens::ui::RealtimePlotWidget;

namespace {

void writeText(const QString& path, const QByteArray& data) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(data);
    f.close();
}

}  // namespace

TEST_CASE("export utils: recursive dir copy preserves layout and content", "[ui][export][tier2]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString src = tmp.path() + QStringLiteral("/src");
    const QString dst = tmp.path() + QStringLiteral("/dst");
    REQUIRE(QDir().mkpath(src + QStringLiteral("/202609")));
    writeText(src + QStringLiteral("/202609/data.db"), "sqlite-bytes-01");
    writeText(src + QStringLiteral("/cfg.json"), "{\"a\":1}");

    QString err;
    REQUIRE(copyDirRecursive(src, dst, &err));
    REQUIRE(QFileInfo::exists(dst + QStringLiteral("/202609/data.db")));
    REQUIRE(QFileInfo::exists(dst + QStringLiteral("/cfg.json")));
    QFile f(dst + QStringLiteral("/202609/data.db"));
    REQUIRE(f.open(QIODevice::ReadOnly));
    REQUIRE(f.readAll() == QByteArrayLiteral("sqlite-bytes-01"));
    f.close();

    // 源缺失 → false + 错误信息
    QString err2;
    REQUIRE_FALSE(copyDirRecursive(tmp.path() + QStringLiteral("/nope"), dst, &err2));
    REQUIRE_FALSE(err2.isEmpty());
}

TEST_CASE("export utils: copy file to dir overwrites existing", "[ui][export][tier2]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString src = tmp.path() + QStringLiteral("/point_table.json");
    const QString dstDir = tmp.path() + QStringLiteral("/out");
    writeText(src, "{\"meta\":{\"schemaVersion\":\"1.1\"}}");
    // 预置同名旧文件 → 覆盖
    REQUIRE(QDir().mkpath(dstDir));
    writeText(dstDir + QStringLiteral("/point_table.json"), "old");

    QString err;
    REQUIRE(copyFileToDir(src, dstDir, &err));
    QFile f(dstDir + QStringLiteral("/point_table.json"));
    REQUIRE(f.open(QIODevice::ReadOnly));
    REQUIRE(f.readAll().startsWith("{\"meta\""));
    f.close();
    REQUIRE_FALSE(copyFileToDir(tmp.path() + QStringLiteral("/missing.json"), dstDir, &err));
}

TEST_CASE("export utils: realtime plot save png writes non-empty file", "[ui][export][tier2]") {
    RealtimePlotWidget w;
    w.resize(600, 300);
    w.show();
    QApplication::processEvents(QEventLoop::AllEvents, 50);

    w.addChannel(1, QStringLiteral("pt 1"), QColor());
    const uint64_t base = 1756800000000ULL;
    for (int i = 0; i < 20; ++i) {
        w.onNewSample(1, 10.0 + i, static_cast<qint64>(base + i * 100));
    }
    w.refreshNow();

    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString png = tmp.path() + QStringLiteral("/curve.png");
    REQUIRE(w.savePng(png));              // FR-EXP-02
    REQUIRE(QFileInfo(png).size() > 0);
    w.close();
}
