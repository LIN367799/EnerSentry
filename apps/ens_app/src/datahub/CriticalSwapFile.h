// src/datahub/CriticalSwapFile.h
// L3 数据中枢 ── 黑匣子 mmap 交换文件（ENS-LLD-200 §3.6.1/3.6.3 / ADR-14 / HLD §3.2.2）。
//
// 设计：
//   * mmap 文件格式:64B Header(magic/version/snapshotCount/writePos) + 环形 Snapshot 槽位
//   * 槽位 = 32B 元数据(pointId/alarmTime/level/count) + 600×16B Sample 数组
//   * appendSnapshot:memcpy(~50μs) + FlushViewOfFile(异步落盘,进程崩溃不丢)
//   * CriticalSwapRecovery::start:文件锁冲突 → backup & recreate(LLD §3.6.3 V1.5)

#pragma once

#include "Sample.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <QString>

#include "platform/PlatformMMap.h"

namespace ens::datahub {

/// 告警等级(对齐 alarm_record.level:0=Info 1=Warning 2=Critical)
enum class AlarmLevel : uint8_t {
    Info     = 0,
    Warning  = 1,
    Critical = 2,
};

/// 黑匣子快照:告警前后 ±30s 的原始 Sample 数组
struct BlackBoxSnapshot {
    uint32_t           pointId   = 0;
    uint64_t           alarmTime = 0;      // 告警触发时间(Unix ms)
    AlarmLevel         level     = AlarmLevel::Info;
    std::vector<Sample> samples;           // 16B × count,raw 数组
};

/// mmap 交换文件(Critical 级即时落盘)
class CriticalSwapFile {
public:
    static constexpr size_t kHeaderSize      = 64;     // Header 固定 64B
    static constexpr size_t kSlotMetaSize    = 32;     // 每槽位元数据 32B
    // ±30s @100ms 含两端点 = 601 样本 > LLD 原文 600(未含端点),取 1024 提供余量
    // (V1.5 如改动态长度可再收紧;2 的幂利于槽位对齐)
    static constexpr size_t kMaxSamplesPerSnap = 1024;
    static constexpr size_t kSlotCount       = 64;     // 环形槽位数
    static constexpr size_t kMaxPending      = 16;     // 启动恢复上限(防 mmap 文件被恶意填充)

    /// 槽位总字节数(元数据 + Sample 数组)
    static size_t slotSize() noexcept {
        return kSlotMetaSize + kMaxSamplesPerSnap * sizeof(Sample);
    }
    /// 文件总字节数
    static size_t totalFileSize() noexcept {
        return kHeaderSize + kSlotCount * slotSize();
    }

    CriticalSwapFile() = default;
    ~CriticalSwapFile() { close(); }

    CriticalSwapFile(const CriticalSwapFile&) = delete;
    CriticalSwapFile& operator=(const CriticalSwapFile&) = delete;

    /// 打开(或创建)交换文件;首开时初始化 Header
    bool open(const QString& path);

    /// 追加一个快照(环形槽位;memcpy + 异步落盘)
    /// @return true 成功
    bool appendSnapshot(const BlackBoxSnapshot& snap);

    /// 解析当前所有未覆盖快照(启动恢复 / 测试验证)
    /// @param limit 最多解析数(默认 kMaxPending)
    std::vector<BlackBoxSnapshot> parsePendingSnapshots(size_t limit = kMaxPending) const;

    /// 诊断:已写快照总数
    uint32_t snapshotCount() const;

    /// 关闭(幂等)
    void close();

private:
    // Header 布局(64B)
    struct Header {
        char     magic[8];        // "ENSBBX01"
        uint32_t version;         // = 1
        uint32_t snapshotCount;   // 已写快照总数
        uint32_t writePos;        // 下一个写槽位(环形)
        uint32_t reserved[11];    // 44B 保留
    };
    static_assert(sizeof(Header) == 64, "Header must be 64 bytes");

    // 槽位元数据布局(32B;pack(1) 防 u64 对齐膨胀)
#pragma pack(push, 1)
    struct SlotMeta {
        uint32_t pointId;
        uint64_t alarmTime;
        uint8_t  level;
        uint32_t count;
        uint8_t  reserved[15];
    };
#pragma pack(pop)
    static_assert(sizeof(SlotMeta) == 32, "SlotMeta must be 32 bytes");

    /// 槽位内 Sample 数组起始地址
    void* slotSamples(size_t slotIdx) const;
    /// 槽位元数据地址
    void* slotMeta(size_t slotIdx) const;

    std::unique_ptr<platform::IMappedFile> m_mmap;
    QString m_path;
    uint32_t m_writePos = 0;
    uint32_t m_snapshotCount = 0;
    bool     m_initialized = false;
};

/// 启动恢复:文件锁冲突下 backup & recreate(LLD §3.6.3 V1.5)
struct RecoveryResult {
    bool     recovered = false;
    int      pendingSnapshots = 0;
    QString  backupPath;
};

class CriticalSwapRecovery {
public:
    /// 尝试打开交换文件;若被锁定则备份旧文件并重建
    /// @return recovered=false 时黑匣子降级(采样继续,mmap 不可用)
    static RecoveryResult start(const QString& swapPath);
};

}  // namespace ens::datahub
