// src/ui/common/ExportUtils.h —— 导出工具函数（切片 41，FR-EXP-02/05/06）。
// 纯文件操作（无 Qt Widget 依赖）→ 可单测。
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace ens::ui {

/// 递归拷贝 srcDir 内容 → dstDir（目标目录自动创建；失败中止返回 false）
inline bool copyDirRecursive(const QString& srcDir, const QString& dstDir, QString* err = nullptr) {
    QDir src(srcDir);
    if (!src.exists()) {
        if (err) *err = QStringLiteral("源目录不存在：%1").arg(srcDir);
        return false;
    }
    QDir dst(dstDir);
    if (!dst.exists() && !dst.mkpath(QStringLiteral("."))) {
        if (err) *err = QStringLiteral("无法创建目标目录：%1").arg(dstDir);
        return false;
    }
    const QStringList entries = src.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& name : entries) {
        const QString sp = src.filePath(name);
        const QString dp = dst.filePath(name);
        if (QFileInfo(sp).isDir()) {
            if (!copyDirRecursive(sp, dp, err)) return false;
        } else {
            if (QFile::exists(dp) && !QFile::remove(dp)) {
                if (err) *err = QStringLiteral("无法覆盖：%1").arg(dp);
                return false;
            }
            if (!QFile::copy(sp, dp)) {
                if (err) *err = QStringLiteral("拷贝失败：%1 → %2").arg(sp, dp);
                return false;
            }
        }
    }
    return true;
}

/// 拷贝单个文件到目录（目标同文件名；失败返回 false 并置 err）
inline bool copyFileToDir(const QString& srcFile, const QString& dstDir, QString* err = nullptr) {
    if (!QFileInfo::exists(srcFile)) {
        if (err) *err = QStringLiteral("源文件不存在：%1").arg(srcFile);
        return false;
    }
    QDir dst(dstDir);
    if (!dst.exists() && !dst.mkpath(QStringLiteral("."))) {
        if (err) *err = QStringLiteral("无法创建目标目录：%1").arg(dstDir);
        return false;
    }
    const QString dp = dst.filePath(QFileInfo(srcFile).fileName());
    if (QFile::exists(dp) && !QFile::remove(dp)) {
        if (err) *err = QStringLiteral("无法覆盖：%1").arg(dp);
        return false;
    }
    if (!QFile::copy(srcFile, dp)) {
        if (err) *err = QStringLiteral("拷贝失败：%1 → %2").arg(srcFile, dp);
        return false;
    }
    return true;
}

}  // namespace ens::ui
