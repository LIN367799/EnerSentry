// src/datahub/CriticalSwapFile.cpp
// L3 数据中枢 ── 黑匣子 mmap 交换文件实现（ENS-LLD-200 §3.6.1/3.6.3）。

#include "CriticalSwapFile.h"

#include <cstring>
#include <memory>

#include <QDateTime>
#include <QFile>
#include <QFileInfo>

namespace ens::datahub {

namespace {

constexpr char kMagic[8] = {'E', 'N', 'S', 'B', 'B', 'X', '0', '1'};

}  // namespace

bool CriticalSwapFile::open(const QString& path) {
    close();                                    // 幂等:重开前先清理

    auto mmap = platform::createMappedFile();   // 非 const:后续需要 move 进成员
    if (!mmap->open(path.toStdString(), totalFileSize(), /*readOnly=*/false)) {
        return false;
    }

    // 检查 header:新文件(全 0)初始化;已有文件验证 magic
    auto* hdr = static_cast<Header*>(mmap->baseAddress());
    const bool isNewFile = (std::memcmp(hdr->magic, kMagic, 8) != 0);
    if (isNewFile) {
        std::memcpy(hdr->magic, kMagic, 8);
        hdr->version       = 1;
        hdr->snapshotCount = 0;
        hdr->writePos      = 0;
        std::memset(hdr->reserved, 0, sizeof(hdr->reserved));
        // 槽位区清零
        std::memset(static_cast<char*>(mmap->baseAddress()) + kHeaderSize,
                    0, totalFileSize() - kHeaderSize);
        mmap->flushAsync(0, totalFileSize());
    } else {
        if (hdr->version != 1) return false;    // 版本不兼容
        // 防损坏:超限值钳制
        if (hdr->writePos >= kSlotCount) hdr->writePos = 0;
    }

    m_mmap = std::move(mmap);
    m_path = path;
    m_writePos      = hdr->writePos;
    m_snapshotCount = hdr->snapshotCount;
    m_initialized   = true;
    return true;
}

void* CriticalSwapFile::slotMeta(size_t slotIdx) const {
    return static_cast<char*>(m_mmap->baseAddress()) + kHeaderSize
           + slotIdx * slotSize();
}

void* CriticalSwapFile::slotSamples(size_t slotIdx) const {
    return static_cast<char*>(slotMeta(slotIdx)) + kSlotMetaSize;
}

bool CriticalSwapFile::appendSnapshot(const BlackBoxSnapshot& snap) {
    if (!m_initialized || m_mmap == nullptr) return false;
    if (snap.samples.size() > kMaxSamplesPerSnap) return false;   // 边界:超长拒写

    const size_t slot = m_writePos % kSlotCount;
    auto* meta = static_cast<SlotMeta*>(slotMeta(slot));
    auto* samples = static_cast<Sample*>(slotSamples(slot));

    meta->pointId   = snap.pointId;
    meta->alarmTime = snap.alarmTime;
    meta->level     = static_cast<uint8_t>(snap.level);
    meta->count     = static_cast<uint32_t>(snap.samples.size());
    std::memset(meta->reserved, 0, sizeof(meta->reserved));
    // memcpy ~50μs:600×16B
    std::memcpy(samples, snap.samples.data(),
                snap.samples.size() * sizeof(Sample));

    // 更新 header + 落盘(异步;进程崩溃不丢,断电极端场景由 flushSync 兜底)
    auto* hdr = static_cast<Header*>(m_mmap->baseAddress());
    ++m_snapshotCount;
    hdr->snapshotCount = m_snapshotCount;
    m_writePos = (m_writePos + 1) % kSlotCount;
    hdr->writePos = m_writePos;

    m_mmap->flushAsync(kHeaderSize + slot * slotSize(),
                       slotSize() + sizeof(Header));
    return true;
}

std::vector<BlackBoxSnapshot> CriticalSwapFile::parsePendingSnapshots(size_t limit) const {
    std::vector<BlackBoxSnapshot> out;
    if (!m_initialized || m_mmap == nullptr) return out;

    const size_t count = std::min(static_cast<size_t>(m_snapshotCount), limit);
    if (m_snapshotCount == 0) return out;

    // 从最老未覆盖槽位开始读(环形:writePos 之前的槽位;越界回绕)
    // 简化:从 0 槽开始顺序读,跳过空槽(count==0)
    size_t read = 0;
    for (size_t i = 0; i < kSlotCount && out.size() < count; ++i) {
        const auto* meta = static_cast<const SlotMeta*>(slotMeta(i));
        if (meta->count == 0 || meta->count > kMaxSamplesPerSnap) continue;
        const auto* samples = static_cast<const Sample*>(slotSamples(i));
        BlackBoxSnapshot snap;
        snap.pointId   = meta->pointId;
        snap.alarmTime = meta->alarmTime;
        snap.level     = static_cast<AlarmLevel>(meta->level);
        snap.samples.assign(samples, samples + meta->count);
        out.push_back(std::move(snap));
        ++read;
    }
    return out;
}

uint32_t CriticalSwapFile::snapshotCount() const {
    return m_snapshotCount;
}

void CriticalSwapFile::close() {
    if (m_mmap != nullptr) {
        m_mmap->flushSync(0, totalFileSize());   // 落盘(幂等)
        m_mmap->close();
        m_mmap.reset();
    }
    m_path.clear();
    m_initialized = false;
    m_writePos = 0;
    m_snapshotCount = 0;
}

// ── CriticalSwapRecovery(LLD §3.6.3 V1.5) ──

RecoveryResult CriticalSwapRecovery::start(const QString& swapPath) {
    RecoveryResult result{};
    CriticalSwapFile swap;
    if (swap.open(swapPath)) {
        result.recovered = true;
        result.pendingSnapshots =
            static_cast<int>(swap.parsePendingSnapshots().size());
        return result;                              // 正常路径
    }

    // 文件被其他进程锁定(Windows ERROR_SHARING_VIOLATION)
    auto mmap = platform::createMappedFile();
    if (!mmap->open(swapPath.toStdString(), CriticalSwapFile::totalFileSize(), false)) {
        if (mmap->isLockedByOtherProcess()) {
            qWarning("CriticalSwapRecovery: swap locked by other process: %s",
                     qUtf8Printable(swapPath));
            if (!QFileInfo::exists(swapPath)) {      // 文件已被接管/清理
                mmap->close();
                return result;                       // 无法重建(上层降级)
            }
            // 备份旧文件 → 重新创建
            const QString backupPath = QStringLiteral("%1.backup_%2")
                .arg(swapPath)
                .arg(QDateTime::currentMSecsSinceEpoch());
            if (QFile::rename(swapPath, backupPath)) {
                qWarning("CriticalSwapRecovery: backed up locked swap to: %s",
                         qUtf8Printable(backupPath));
                result.backupPath = backupPath;
                swap.open(swapPath);                 // 重建(新文件)
                result.recovered = true;
                result.pendingSnapshots = 0;
                return result;
            }
            // 备份失败 → 删除重建(记录数据丢失风险)
            qCritical("CriticalSwapRecovery: backup failed, recreating (DATA LOSS RISK): %s",
                      qUtf8Printable(swapPath));
            QFile::remove(swapPath);
            swap.open(swapPath);
            result.recovered = true;
            result.pendingSnapshots = 0;
            return result;
        }
        // 其他错误(权限/磁盘满)→ 黑匣子降级,采样继续
        qCritical("CriticalSwapRecovery: swap open failed (err=%d): %s → degraded",
                  mmap->lastError(), qUtf8Printable(swapPath));
        return result;
    }
    mmap->close();
    return result;
}

}  // namespace ens::datahub
