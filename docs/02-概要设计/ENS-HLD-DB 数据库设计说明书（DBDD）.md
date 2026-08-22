# EnerSentry 储能上位机系统 —— 数据库设计说明书（DBDD）

> **文档编号**：ENS-DBDD-001　**版本**：V1.7　**日期**：2026-08-12　**状态**：修订稿 / 待复审
> **前序文档**：ENS-HLD-001《概要设计说明书（HLD V1.5）》
> **作者**：数据与业务开发工程师

---

## 1. 引言与设计目标


### 1.1 编写目的

本文档是 EnerSentry 储能上位机系统《概要设计说明书（HLD V1.5）》的配套落地文档，旨在将概要设计中"分级存储、按月分库、WAL 批量落库、黑匣子快照"等架构决策，转化为**可直接执行的 SQL DDL** 与**可编译的 C++17 数据访问层接口**，作为数据库实现、联调与运维的唯一权威依据。

本文档服务于以下角色与活动：

- **数据开发工程师**：依据 §4 DDL 建表、依据 §3/§6 实现路由与 DAL；
- **测试工程师**：依据 §5 量化指标设计压测与断电故障注入用例；
- **运维工程师**：依据 §2.4 目录布局与 §5.3 磁盘熔断策略进行部署与容量规划。

### 1.2 存储架构量化指标映射

下列指标直接映射自 SRS（FR-DLM-01~08、FR-AL-01~13、NFR-PERF-12）与 HLD（§3.2 分级存储、§5 性能）。

| 指标 | 目标值 | 来源 | 本文档落点 |
|------|--------|------|-----------|
| 高频采集粒度 | 100ms（每测点每 100ms 一帧） | FR-DLM-02 | §2.2 L1 RingBuffer |
| 落库吞吐 | **≥ 5,000 点/秒** 持续写入，采集线程不阻塞 | FR-DLM-06 / NFR-PERF-12 | §5.1 WAL + §5.2 批量事务 |
| L1 保留周期 | 最近 1 小时 100ms 全量 | FR-DLM-02 | §2.2 RingBuffer 容量 = 36000 槽 |
| L2 保留周期 | 180 天（按月库自动归档） | FR-DLM-02 | §2.4 + §5.3 |
| 降采样粒度 | 1s / 5s / 1m（保留 Max/Min/Avg + 原始计数） | FR-DLM-03 | §4.2 |
| 黑匣子窗口 | Critical 告警 ±30s 高频数据 | FR-DLM-07 | §2.3 + §4.3 |
| 黑匣子保留 | 永久（独立 `blackbox.db`） | FR-DLM-08 | §2.4 + §4.3 |
| 告警分级 | Info / Warning / Critical 三级 | FR-AL-02 | §4.4 `level` |
| 同源抑制窗口 | 默认 60s | FR-AL-04 | §4.4 `last_alarm_ts`（业务层） |
| 延时确认 | 默认 3s | FR-AL-05 | §4.4 `status` 状态机 |
| 告警记录保留 | ≥ 365 天（月度归档，不自动删） | FR-AL-11 | §2.4 + §4.4 |
| 审计日志保留 | ≥ 3 年（不可篡改，仅管理员可见） | FR-AUTH-04 | §2.4 + §4.5 |
| 跨月查询上限 | 单次 ≤ 3 个月 | HLD V1.4 | §3.3 `MAX_CROSS_MONTHS_PER_QUERY` |
| 单查询结果上限 | ≤ 50 万行 | HLD V1.3 | §3.3 `MAX_QUERY_ROWS` |
| 断电恢复 | WAL 文件下次启动自动恢复，已 commit 数据不丢失 | NFR-REL-04 | §5.1 + §2.3 Torn-Write 防护 |

### 1.3 设计原则

1. **高频不落全量、低频不丢趋势**（HLD/蓝图）：100ms 全量仅驻 L1 与黑匣子；L2 仅存降采样聚合。
2. **读写分离 + 连接池隔离**：写入连接池只做 `batchInsert`；跨月只读查询走独立只读连接池 + `ATTACH DATABASE`。
3. **崩溃一致性优先**：WAL `synchronous=NORMAL` 提供足够持久性；mmap 交换文件采用 Magic Number + CRC32 + Double Header 防御 Torn Write。
4. **归档即删文件**：按月分库后，过期数据通过移动/删除整库文件完成，避免 `DELETE` 锁库与碎片化。
5. **接口解耦**：所有存储细节经 `IDataAccess` 纯虚基类屏蔽，业务层零改动即可替换存储策略（`monthly` / `single_table`）。

---

## 2. 存储总体架构与分级存储设计

### 2.1 L1 / L2 拓扑

```
                ┌─────────────────────────── 采集热路径（无锁） ───────────────────────────┐
 设备/RTU/TCP ──▶ 协议引擎解析 ──▶ RingBuffer<Sample>(L1, alignas(16), 无锁)
                │                   │  push(publish指针+release屏障)
                │                   │
                │                   ├─▶ 渲染线程 getRecent() 实时曲线（100ms）
                │                   ├─▶ 降采样线程 DownSampler（1s/5s/1m 窗口聚合）
                │                   │        │
                │                   │        ▼
                │                   │   Batch Insert（100ms/1000条 事务）──▶ L2 SQLite WAL
                │                   │
                │                   └─▶ 告警引擎越限判定（延时确认+同源抑制）
                │                            │ Critical 成立
                │                            ▼
                │                   BlackBoxManager.triggerBlackBox()
                │                       ├─▶ mmap critical_swap.dat（IMappedFile, 100MB 循环, 即时落盘）
                │                       └─▶ 异步 persistSnapshot() ──▶ L2 blackbox.db（永久）
                └─────────────────────────────────────────────────────────────────────────────┘

  L1: 内存高频快照（1h, 100ms）       L2: SQLite WAL 历史库（180天, 降采样）
       关键测点锁定 ±30s ──黑匣子──▶ 永久保留
```

**分层职责表**

| 层 | 介质 | 数据 | 写入者 | 读取者 |
|----|------|------|--------|--------|
| L1 | 进程内存 RingBuffer | 100ms 全量 + 黑匣子锁定段 | 采集线程（无锁 push） | 渲染线程、降采样线程、告警引擎、黑匣子 |
| Critical 屏障 | `critical_swap.dat` (mmap) | Critical 告警 ±30s 原始帧 | 黑匣子管理器（1ms 级） | 断电恢复 / L2 回放 |
| L2 历史 | `data_YYYYMM.db` (WAL) | 1s/5s/1m 降采样 | 持久化线程（批量事务） | 趋势查询（UI/报表） |
| 黑匣子 | `blackbox.db` | ±30s 高频 BLOB 快照 | 黑匣子管理器（异步） | 告警回放 |
| 告警 | `alarm_YYYYMM.db` 内 `alarm_record_YYYYMM`（V1.5 独立） | 告警全生命周期 | `PersistThread` 消费队列（推荐） | 告警中心 |
| 审计 | `audit_YYYYMM.db` | 写操作留痕 | 业务层/控制层 | 审计查询（管理员） |

### 2.2 L1 RingBuffer：`Sample` 与无锁发布

L1 高频采样使用 `alignas(16)` 的 `Sample` 结构体，单个槽位恰好 16 字节，可在 x86-64 上以单条写指令原子提交，配合发布指针（publishedPos）实现多生产者-多消费者无锁读取，避免撕裂读与回卷覆盖（HLD ADR-08）。

```cpp
// datahub/Sample.h —— L1 高频采样单元（16 字节，Cache Line 友好）
#pragma once
#include <cstdint>
#include <atomic>

#if defined(_MSC_VER)
    #define ENS_CACHE_ALIGN __declspec(align(16))
#elif defined(__GNUC__) || defined(__clang__)
    #define ENS_CACHE_ALIGN __attribute__((aligned(16)))
#else
    #define ENS_CACHE_ALIGN alignas(16)
#endif

struct ENS_CACHE_ALIGN Sample {
    uint64_t timestamp;   // Unix 毫秒时间戳（8B，地址 8 的倍数）
    uint32_t pointId;     // 测点 ID   （4B）
    float    value;       // 采样值    （4B）  ← 恰填满 16 字节
};
static_assert(sizeof(Sample) == 16, "Sample must be 16 bytes for atomic access");

// 【V1.4】跨平台 lock-free 编译期断言：防止 32-bit x86 / 部分 ARM 上退化为互斥锁
static_assert(std::atomic<Sample>::is_always_lock_free,
              "Sample (16B aligned) must be lock-free on this platform! "
              "Check: x86-64 OK; 32-bit x86 / ARMv7 may fail. "
              "Fallback: shrink to 8B (use uint32 timestamp instead of uint64).");

// 黑匣子扩展结构（黑匣子回放时携带，平时默认 0）
struct SampleWithMeta {
    Sample   core;            //  16B 原子块
    uint64_t blackBoxId;      // 仅黑匣子使用
};
static_assert(sizeof(SampleWithMeta) == 24, "SampleWithMeta layout check");
```

**无锁生产者（发布指针语义）**

```cpp
// datahub/RingBuffer.h（节选：L1 高频环形缓冲，容量 = 36000 槽 = 100ms × 3600s）
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : m_capacity(capacity),
        m_buffer(std::make_unique<T[]>(capacity)) {}

    // 生产者（采集线程）：Store-Store 屏障 + 提交写指针
    void push(const T& item) {
        size_t pos = m_writePos.fetch_add(1, std::memory_order_relaxed) % m_capacity;
        m_buffer[pos] = item;                                       // ① 写数据 (relaxed)
        std::atomic_thread_fence(std::memory_order_release);       // ② release 屏障
        m_publishedPos.store(pos, std::memory_order_release);       // ③ 发布已提交槽位
    }

    // 消费者（渲染/降采样/告警）：acquire 语义读取最新已发布边界
    std::vector<T> readRecent(size_t count) const {
        size_t published = m_publishedPos.load(std::memory_order_acquire);
        size_t currentPos = m_writePos.load(std::memory_order_relaxed);
        // 仅遍历 [currentPos - count, currentPos) 已发布区间，避免中间态
        // ...
    }

    size_t capacity() const { return m_capacity; }

private:
    size_t m_capacity;
    std::unique_ptr<T[]> m_buffer;
    std::atomic<size_t> m_writePos{0};
    std::atomic<size_t> m_publishedPos{0};   // 二级发布指针（HLD ADR-08）
};

// L1 高频缓冲容量 = 36000 槽（100ms × 3600s = 1h）；低频 = 3600 槽（1s × 3600s）
// 见 HLD §5.3 L1SnapshotStore
```

> **容量依据**：100ms × 3600s = 36000 槽 × 16B ≈ 576 KB / 测点；按 ≤ 200 关键高频测点计，L1 内存占用 < 120 MB，远低于 L3 缓存预算（NFR-MEM）。

### 2.3 Critical 极速屏障：`IMappedFile` 与 `critical_swap.dat` 二进制格式

Critical 告警触发时，必须在**毫秒级**将 ±30s 高频数据持久化到磁盘，且**断电不丢失**（HLD §3.2.3）。采用 `IMappedFile` 抽象（Win32 `CreateFileMapping` / POSIX `mmap`）映射 100MB 文件，进程内直接写即同步内核页缓存；独立 Fsync 线程每 200ms `msync(MS_ASYNC)` 做常规落盘，**Critical 告警触发瞬间同步调用 `syncBlocking()`（`msync(MS_SYNC)`）**确保硬断电安全；文件写满循环覆盖最旧数据。

> **V1.5 评审澄清**：`MS_ASYNC` 仅将内核页缓存标记为脏，不保证立即落入物理介质；瞬间硬断电（无 UPS/超级电容）时，最近 200ms 内的异步刷盘数据仍可能丢失。Critical 路径在触发时刻执行一次阻塞同步（`MS_SYNC` / `FlushFileBuffers`），单次写入量仅数 KB，实测耗时通常 **< 2ms**，以极低时延代价换取真正的硬断电安全。

**二进制文件布局**

```cpp
// datahub/CriticalSwapFile.h —— Critical 极速屏障交换文件（100MB 循环覆盖）
#pragma once
#include <cstdint>
#include <atomic>
#include <QString>

class CriticalSwapFile {
public:
    static constexpr size_t SWAP_FILE_SIZE = 100 * 1024 * 1024;  // 100MB（≈ 1h Critical 缓冲）
    static constexpr size_t SLOT_SIZE      = 8 * 1024;            // 8KB / 快照槽位

    // 文件首 4KB = 索引区（原子字段，受 Torn-Write 防护）
    struct SwapHeader {
        uint64_t magic;            // 魔数 0x4553534343525400ULL ("ESSCRT\0")
        uint64_t version;          // 格式版本
        uint64_t writePos;         // 当前写入偏移 (atomic)
        uint64_t snapshotCount;    // 累计写入数 (atomic)
        uint64_t pendingL2Sync;    // 未上传 L2 的快照数 (atomic)
        char reserved[4064];
    };
    static_assert(sizeof(SwapHeader) == 4096, "SwapHeader must be 4KB");

    // 单个 8KB 槽位
    struct SwapSlot {
        uint64_t alarmId;        // 关联告警 ID
        uint64_t alarmTimeMs;    // 告警时间（Unix ms）
        uint32_t pointId;        // 触发点 ID
        uint32_t sampleCount;    // 实际样本数
        uint8_t  level;          // 告警级别（应 = Critical=2）
        uint8_t  padding[3];
        uint8_t  samples[8096 - 32];   // 实际样本载荷（Sample 数组，16B/个，最多 506 个）
    };
    static_assert(sizeof(SwapSlot) == SLOT_SIZE, "Slot size mismatch");

    bool open(const QString& path);
    void close();
    SwapSlot* allocateSlot();                  // 原子预占槽位（< 1μs）
    void markCommittedToL2(uint64_t alarmId);  // 标记已上传
    std::vector<SwapSlot*> recoverPending();    // 启动时恢复未提交快照
    void syncBlocking();                        // msync(MS_SYNC)，≤ 20ms（断电前窗口调用）

private:
    int      m_fd = -1;
    char*    m_baseAddr = nullptr;
    SwapHeader* m_header = nullptr;
};
```

**`IMappedFile` 抽象（平台无关 mmap 封装）**

```cpp
// platform/IMappedFile.h —— 跨平台内存映射文件抽象
#pragma once
#include <cstdint>
#include <string>

class IMappedFile {
public:
    virtual ~IMappedFile() = default;

    // 创建/打开并映射 length 字节；返回映射基址，失败返回 nullptr
    virtual void* map(const std::string& path, size_t length, bool readOnly = false) = 0;
    virtual void  unmap() = 0;

    // 异步刷盘（不阻塞）；sync 可选强制同步
    virtual bool  msync(bool sync = false) = 0;

    // 预分配物理空间（避免写时缺页抖动）
    virtual bool  preallocate(size_t length) = 0;
};

// 工厂：按编译平台返回具体实现
std::unique_ptr<IMappedFile> createMappedFile();
```

**Windows 实现（节选）**

```cpp
// platform/Win32MMap.h
class Win32MMap : public IMappedFile {
public:
    void* map(const std::string& path, size_t length, bool readOnly) override {
        m_hFile = CreateFileA(path.c_str(),
                              readOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE),
                              FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_hFile == INVALID_HANDLE_VALUE) return nullptr;
        m_hMap = CreateFileMapping(m_hFile, nullptr,
                                   readOnly ? PAGE_READONLY : PAGE_READWRITE,
                                   (length >> 32), (length & 0xFFFFFFFF), nullptr);
        if (!m_hMap) return nullptr;
        m_base = MapViewOfFile(m_hMap,
                               readOnly ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS,
                               0, 0, length);
        m_length = length;
        return m_base;
    }
    bool msync(bool sync) override {
        return FlushViewOfFile(m_base, m_length) &&
               (sync ? (FlushFileBuffers(m_hFile) != 0) : true);
    }
    // ... unmap / preallocate 实现省略
};
```

**mmap Torn-Write 防护（V1.2）：Magic Number + CRC32 + Double Header**

为防止断电时 Header 指针与载荷不一致导致解析到半写数据（误报告警），每个快照采用 `SnapshotHeader`（32 字节，Cache Line 对齐）描述，写入顺序保证 **Magic Number 最后写 = 原子提交点**；并在载荷后写入第二份 Header 副本（Double Header）用于主 Header 损坏时恢复。

```cpp
// datahub/SnapshotHeader.h —— V1.2 崩溃一致性 Snapshot Header
#pragma once
#include <cstdint>
#include <cstring>

namespace ens::datahub {
static constexpr uint32_t SNAPSHOT_MAGIC = 0x454E5353; // "ENS\0" little-endian

struct alignas(32) SnapshotHeader {
    uint32_t magic;         // = SNAPSHOT_MAGIC
    uint16_t version;       // Header 版本号（当前 = 1）
    uint16_t flags;         // bit0=valid, bit1=crc_enabled, bit2=double_header
    uint64_t alarmId;       // 关联 Critical 告警 ID
    uint64_t timestampMs;   // 快照创建时刻（Unix ms）
    uint32_t sampleCount;   // 载荷样本数
    uint32_t payloadOffset; // 载荷相对 mmap 基址偏移（字节）
    uint32_t payloadSize;   // 载荷总大小（字节）
    uint32_t headerCrc32;   // Header 自身 CRC32 校验和
    static constexpr size_t HEADER_SIZE = 32;
};

// CRC-32 (IEEE 802.3) 多项式 0xEDB88320，无第三方依赖
inline uint32_t computeHeaderCrc32(const SnapshotHeader& hdr) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&hdr);
    for (size_t i = 0; i < offsetof(SnapshotHeader, headerCrc32); ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return crc ^ 0xFFFFFFFF;
}
inline uint32_t computePayloadCrc32(const void* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return crc ^ 0xFFFFFFFF;
}
} // namespace ens::datahub
```

**原子写入顺序（崩溃一致性保证）**

```
Step 1: 计算 payloadBytes = count * sizeof(Sample)
Step 2: memcpy(primaryHdr)  ← Header 元数据（magic = 0，即"未提交"）
Step 3: primaryHdr->magic = SNAPSHOT_MAGIC  ← ★ 原子提交点
Step 4: memcpy(secondaryHdr) ← Double Header 副本
Step 5: secondaryHdr->magic = SNAPSHOT_MAGIC
```

> 断电若发生在 Step 2~3 之间：`magic == 0` → 解析器跳过该槽位（视为空）✅ 安全；
> 若主 Header CRC 不匹配 → 自动从 Double Header 副本恢复。详见非功能保障设计说明 §3.2.4。

**Critical 触发时的阻塞同步调用点**

```cpp
// datahub/CriticalSwapFile.cpp
void CriticalSwapFile::commitCriticalSnapshot(uint64_t alarmId,
                                              const std::vector<Sample>& samples) {
    SwapSlot* slot = allocateSlot();
    // ... 填充 slot->alarmId / alarmTimeMs / pointId / samples ...
    // ... 按原子写入顺序写入 SnapshotHeader（magic 最后写）...

    // V1.5：Critical 告警触发瞬间必须阻塞同步到物理介质，确保硬断电不丢失。
    // 单次写入量仅数 KB，实测 < 2ms（远高于 100ms 采样间隔要求）。
    syncBlocking();   // msync(MS_SYNC) / FlushFileBuffers

    // 随后由 L2 持久化线程异步搬运到 blackbox.db（非关键路径）
    markCommittedToL2(alarmId);
}
```

> **运行期策略**：常规后台 Fsync 线程仍每 200ms `msync(MS_ASYNC)` 批量刷盘；仅在 Critical 告警触发时执行一次 `syncBlocking()`。这样平衡了常态性能与事故场景下的硬断电安全。

### 2.4 L2 物理文件目录

按功能与保留周期拆分独立库文件，便于归档（移动/删除整库文件）、备份与权限隔离。

```text
data/
├── history/
│   ├── 202608/
│   │   ├── data_202608.db          # 2026 年 8 月历史库（仅 history_1s/5s/1m_202608）
│   │   └── data_202608.db-wal      # WAL 文件（同目录）
│   ├── 202609/
│   │   └── data_202609.db
│   └── ...                         # 每月一个，过期整库归档/删除
├── alarm/
│   ├── 202608/
│   │   ├── alarm_202608.db         # 2026 年 8 月告警库（仅 alarm_record_202608）
│   │   └── alarm_202608.db-wal
│   └── ...                         # 按月分区，保留 ≥ 365 天
├── blackbox/
│   └── blackbox.db                 # 黑匣子快照，永久保留，独立 DB
├── audit/
│   └── audit_202608.db             # 审计日志，按月分区，保留 ≥ 3 年
├── meta.db                         # 站点元数据、点表配置 point_table、用户/角色、全局 KV
└── critical_swap.dat               # Critical 极速屏障交换文件（100MB，mmap）
```

**保留与归档策略**

| 文件 | 保留周期 | 归档方式 |
|------|----------|----------|
| `history/YYYYMM/data_YYYYMM.db` | 180 天 | 超过 180 天的月份库整体移入冷存储后删除原文件（§5.3 熔断触发或定时任务） |
| `blackbox/blackbox.db` | 永久 | 不自动删除 |
| `audit/audit_YYYYMM.db` | ≥ 3 年 | 仅管理员可清理 |
| `meta.db` | 永久 | 点表热加载、用户/角色不删 |

**冷存储归档前的句柄释放流程（V1.6 评审补充）**

直接删除仍被只读连接池或外部查询持有的 `.db` / `.db-wal` 文件，在 Windows 下会报 `Access Denied`，在 Linux 下会触发 inode 引用计数导致磁盘空间无法释放。归档清理必须执行以下步骤：

1. **路由层加锁**：`ArchiveManager` 获取目标月份的 `MonthArchiveMutex`，阻止新的只读请求进入。
2. **句柄驱逐**：
   - 调用 `ReadOnlyConnectionPool::evict(dbPath)`，强制关闭所有以该路径（含 `.db` 与 `.db-wal`）为 `main` 或 `ATTACH` 别名的空闲连接；
   - 调用 `SQLiteDataAccess::closeConnection(dbPath)`，关闭写入连接池中的对应句柄；
   - 对 `critical_swap.dat` 等 mmap 文件，调用 `IMappedFile::flush()` 后 `unmap()`。
3. **显式 `close()`**：所有 `QSqlDatabase` / 原生 `sqlite3*` 在删除前必须 `close()`，Qt 的 `QSqlDatabase::removeDatabase()` 需在连接不再被任何 `QSqlQuery` 引用后执行。
4. **引用计数归零检查**：使用 `FileHandleTracker`（封装 `GetProcessHandleCount` / `/proc/self/fd` 扫描）确认当前进程对该 `.db` 与 `.db-wal` 的句柄数为 0。
5. **原子迁移 + 删除**：先 `rename()` / `MoveFileEx` 到冷存储目录，成功后 `remove()` 原目录下空文件夹；若迁移失败则回滚并告警。

```cpp
// datahub/ArchiveManager.cpp（V1.6：归档前句柄释放）
bool ArchiveManager::archiveMonth(const QString& monthTag) {
    QString dbPath  = dataRoot + "/history/" + monthTag + "/data_" + monthTag + ".db";
    QString walPath = dbPath + "-wal";

    // 1) 阻止新请求进入该月库
    std::unique_lock<std::shared_mutex> lock(m_archiveMutex[monthTag]);

    // 2) 驱逐只读池与写入池中的该库句柄
    m_readOnlyPool.evict(dbPath);          // 关闭 idle 连接；标记 active 连接为待关闭
    m_dataAccess->closeConnection(dbPath); // 关闭写入连接

    // 3) 等待所有活跃只读查询结束（带超时，避免无限阻塞）
    if (!m_readOnlyPool.waitForZeroActive(dbPath, 10000)) {
        logError("archiveMonth %s: active read connections still hold handles after 10s", qPrintable(monthTag));
        return false;
    }

    // 4) 引用计数归零检查
    if (!FileHandleTracker::isUnused(dbPath) || !FileHandleTracker::isUnused(walPath)) {
        logError("archiveMonth %s: file handles still open", qPrintable(monthTag));
        return false;
    }

    // 5) 原子迁移到冷存储
    QDir coldDir(dataRoot + "/cold/history/" + monthTag);
    coldDir.mkpath(".");
    QString destDb  = coldDir.absoluteFilePath("data_" + monthTag + ".db");
    QString destWal = destDb + "-wal";
    if (!QFile::rename(dbPath, destDb) || !QFile::rename(walPath, destWal)) {
        logError("archiveMonth %s: failed to move files to cold storage", qPrintable(monthTag));
        return false;
    }

    // 6) 删除空目录（可选）
    QDir(monthDir).rmdir(".");
    return true;
}
```

---

## 3. 按月分库与路由策略

### 3.1 `data_YYYYMM.db` 目录与表名后缀

每个自然月一个独立数据库文件：
- 历史库 `data_YYYYMM.db` 内三张降采样表：`history_1s_YYYYMM` / `history_5s_YYYYMM` / `history_1m_YYYYMM`；
- 告警库 `alarm_YYYYMM.db`（V1.5 静态隔离）内 `alarm_record_YYYYMM`；
- 审计库 `audit_YYYYMM.db` 内 `audit_log_YYYYMM`（HLD V1.1）。

**分库收益对比（HLD §3.2.4.1）**

| 指标 | 单表方案（V1.0 草案） | 单月分库（V1.1 终版） |
|------|---------------------|----------------------|
| 单表最大行数 | 100 亿+ | 每库 ≤ 3 亿（1min 粒度） |
| `VACUUM` 耗时 | 小时级、阻塞写入 | 每库 ≤ 5 秒 |
| 范围查询 B-Tree 高度 | 6+ | ≤ 4 + 多库并行 |
| 数据归档 | `DELETE` 极慢、碎片化 | 直接删除 `data_YYYYMM.db` 文件 |
| 异地备份 | 整库几十 GB | 按月增量分发 |

### 3.2 `IDataAccess::getTableName` 路由

所有 SQL 表名前缀由 `getTableName()` 动态拼装，业务层不直接拼接月份字符串，降低耦合。

```cpp
// datahub/SQLiteDataAccess.cpp（节选：路由实现）
QString SQLiteDataAccess::getTableName(uint32_t pointId, uint64_t timestamp,
                                        HistoryGranularity gran) const {
    Q_UNUSED(pointId);                       // 预留：未来按测点分片
    QDateTime dt = QDateTime::fromMSecsSinceEpoch(timestamp);
    QString suffix = dt.toString("yyyyMM");
    switch (gran) {
        case HistoryGranularity::Gran1s:  return QString("history_1s_%1").arg(suffix);
        case HistoryGranularity::Gran5s:  return QString("history_5s_%1").arg(suffix);
        case HistoryGranularity::Gran1m:  return QString("history_1m_%1").arg(suffix);
        case HistoryGranularity::Gran100ms: return QString("history_100ms_%1").arg(suffix);
    }
    return {};
}

QString SQLiteDataAccess::getDatabasePath(uint64_t timestamp) const {
    QDateTime dt = QDateTime::fromMSecsSinceEpoch(timestamp);
    QString monthDir = m_dataRootDir + "/history/" + dt.toString("yyyyMM");
    QDir().mkpath(monthDir);                 // 首次访问时创建目录
    return monthDir + "/data_" + dt.toString("yyyyMM") + ".db";
}

// V1.5：告警/审计独立月库路由
QString SQLiteDataAccess::getAlarmDatabasePath(uint64_t timestamp) const {
    QDateTime dt = QDateTime::fromMSecsSinceEpoch(timestamp);
    QString monthDir = m_dataRootDir + "/alarm/" + dt.toString("yyyyMM");
    QDir().mkpath(monthDir);
    return monthDir + "/alarm_" + dt.toString("yyyyMM") + ".db";
}

QString SQLiteDataAccess::getAuditDatabasePath(uint64_t timestamp) const {
    QDateTime dt = QDateTime::fromMSecsSinceEpoch(timestamp);
    QString monthDir = m_dataRootDir + "/audit/" + dt.toString("yyyyMM");
    QDir().mkpath(monthDir);
    return monthDir + "/audit_" + dt.toString("yyyyMM") + ".db";
}
```

**批量写入按月份分桶**

```cpp
// 落库前按月份分桶，对每个月份 DB 独立开启事务 + prepared statement 批量 INSERT
bool SQLiteDataAccess::batchInsertHistory(const std::vector<DownSampledSample>& samples,
                                          bool isBackfill) {
    if (samples.empty()) return true;
    std::unordered_map<QString, std::vector<DownSampledSample>> buckets;
    for (const auto& s : samples) {
        buckets[getDatabasePath(s.timestamp)].push_back(s);
    }
    for (auto& [dbPath, batch] : buckets) {
        auto db = getOrOpenConnection(dbPath);          // 连接池维护
        if (!db) return false;

        // V1.5：历史表使用 WITHOUT ROWID + PRIMARY KEY(point_id, ts)；
        // 批量写入前必须按 (point_id, ts) 升序排序，避免乱序/补算数据导致 B-Tree 页分裂。
        std::sort(batch.begin(), batch.end(),
                  [](const DownSampledSample& a, const DownSampledSample& b) {
                      return a.pointId < b.pointId ||
                             (a.pointId == b.pointId && a.timestamp < b.timestamp);
                  });

        // V1.7：检测到补算/乱序批次时，临时提升 cache_size 并加日志/指标，
        // 事后在低峰期触发 PRAGMA optimize 或 REINDEX，修复可能产生的页分裂。
        const bool outOfOrder = isBackfill || hasOutOfOrderInBatch(batch);
        if (outOfOrder) {
            db->exec("PRAGMA cache_size = -256000");    // 256MB 页缓存，降低分裂重组 I/O
            emit backfillBatchDetected(dbPath, batch.size());
        }

        QString table = getTableName(/*pointId=*/0, batch.front().timestamp, Gran1s);
        db->exec("BEGIN IMMEDIATE");   // V1.1：直接申请 Reserved 锁，避免并发死锁/BUSY
        // V1.4：标准 INSERT（窗口单向推进，重复 (point_id, ts) 概率极低）；
        // 若业务层明确需要覆写（补算），改为 INSERT ... ON CONFLICT(point_id, ts) DO UPDATE SET ...
        // prepared statement: INSERT INTO <table> (point_id, ts, v_max, v_min, v_avg, sample_count)
        //                     VALUES (?, ?, ?, ?, ?, ?);
        for (const auto& s : batch) { /* bind + step */ }
        db->exec("COMMIT");

        if (outOfOrder) {
            db->exec("PRAGMA optimize");   // SQLite 3.18+：提示 ANALYZE 更新统计信息
        }
    }
    return true;
}

// V1.7：判定当前 batch 是否包含已存在库时间窗口之前的“旧数据”
static bool hasOutOfOrderInBatch(const std::vector<DownSampledSample>& batch) {
    if (batch.empty()) return false;
    const uint64_t oldestTs = batch.front().timestamp;   // 已按 (point_id, ts) 排序
    // 取该批次所属月份库当前已落库的最老/最新时间戳，由 SQLiteDataAccess 缓存
    const uint64_t dbHighWaterMark = SQLiteDataAccess::instance().highWaterMark(
        getDatabasePath(oldestTs));
    // 若 batch 中任何窗口早于 highWaterMark，则视为可能插入 B-Tree 中间节点
    return oldestTs < dbHighWaterMark;
}
```

### 3.2.1 写入互斥控制：单线程单库原则（V1.5 静态隔离）

> **评审关注**：`getOrOpenConnection(dbPath)` 会为每个月库维护写入连接。如果 `PersistThread`（降采样历史）、告警引擎（`alarm_record`）、审计模块（`audit_log`）同时在不同线程中对同一个 `data_YYYYMM.db` 执行 `BEGIN IMMEDIATE`，SQLite 底层仍会触发**文件级写锁互斥**，导致其中一方触发 `busy_timeout` 排队，极端情况下造成写入抖动或告警确认延迟。
>
> **V1.5 决策**：储能系统对告警日志可靠性要求极高，运行时动态切换库路径会带来架构不确定性（`queryAlarms` 需同时检查 `data_YYYYMM.db` 与 `alarm_YYYYMM.db`）。因此告警库采用**静态隔离**：`alarm_record_YYYYMM` **默认且固定**存放在独立 `alarm_YYYYMM.db` 中，由统一的 `PersistThread`（或独立 `AlarmPersistThread`）串行写入。

**设计原则**

| 月库 | 物理文件 | 唯一写入者 | 其他生产者行为 |
|------|---------|-----------|---------------|
| 历史表 | `data_YYYYMM.db` | **`PersistThread`** | 禁止其他线程直接写入 |
| 告警表 | `alarm_YYYYMM.db`（独立） | **`PersistThread` 消费队列**（推荐）或 `AlarmPersistThread` | 告警引擎仅投递 `AlarmRecord` 消息 |
| 审计表 | `audit_YYYYMM.db`（独立） | **`PersistThread` 消费队列**（推荐）或 `AuditPersistThread` | 业务模块仅投递 `AuditLog` 消息 |
| 黑匣子 | `blackbox.db` | **`BlackBoxManager`**（Critical 告警路径，低频） | 直接写入，但不在历史事务内 |

**实现方式（推荐：单 PersistThread 统一消费三类消息，按库分离连接）**

```cpp
// datahub/PersistThread.h（V1.5：单线程统一消费，但历史/告警/审计分别写不同月库）
class PersistThread {
public:
    void run() {
        while (!m_stop) {
            // 1) 历史降采样（最高优先级、固定周期）→ data_YYYYMM.db
            auto histBatch = m_historyBuffer.swap();
            if (!histBatch.empty()) {
                // V1.5：批量写入前按 (point_id, ts) 排序，避免 WITHOUT ROWID 页分裂
                std::sort(histBatch.begin(), histBatch.end(),
                          [](const DownSampledSample& a, const DownSampledSample& b) {
                              return a.pointId < b.pointId ||
                                     (a.pointId == b.pointId && a.timestamp < b.timestamp);
                          });
                m_dal->batchInsertHistory(std::move(histBatch));
            }

            // 2) 告警记录 → alarm_YYYYMM.db（独立库，避免阻塞历史事务）
            std::vector<AlarmRecord> alarms;
            m_alarmQueue.drain(alarms, /*max=*/256);
            for (auto& a : alarms) m_dal->insertAlarm(std::move(a));

            // 3) 审计日志 → audit_YYYYMM.db（独立库）
            std::vector<AuditLog> audits;
            m_auditQueue.drain(audits, /*max=*/256);
            for (auto& au : audits) m_dal->insertAuditLog(std::move(au));

            std::this_thread::sleep_for(std::chrono::milliseconds(
                RuntimeConfig::instance().persistFlushIntervalMs()));
        }
    }
private:
    DoubleBuffer<DownSampledSample> m_historyBuffer;
    MpscQueue<AlarmRecord>          m_alarmQueue;
    MpscQueue<AuditLog>             m_auditQueue;
    IDataAccess*                    m_dal;
};
```

> **路由简化**：由于告警表固定位于 `alarm_YYYYMM.db`，`IDataAccess::insertAlarm` 与 `queryAlarms` 无需运行时判断共享/隔离模式，跨月查询也只需要 `ATTACH` 独立的 `alarm_YYYYMM.db`，与 `data_YYYYMM.db` 完全解耦。

### 3.3 跨月查询 `ATTACH DATABASE` 与只读连接池（V1.3 工业落地优化）

跨月趋势（如 2026-08-25 ~ 2026-09-05）通过 `ATTACH DATABASE` 在一个连接内挂载多月库，以单条 `UNION ALL` 交由 SQLite 引擎归并，避免串行打开多库 + 内存合并排序（HLD §3.2.4.2）。

```cpp
// datahub/SQLiteDataAccess.cpp（节选：跨月查询）
constexpr int MAX_CROSS_MONTHS_PER_QUERY = 3;   // V1.4：单次最大跨月数
constexpr int MAX_QUERY_ROWS             = 500000;

std::vector<DownSampledSample> SQLiteDataAccess::queryHistoryRange(
    uint32_t pointId, uint64_t startTime, uint64_t endTime)
{
    auto monthRanges = splitByMonth(startTime, endTime);
    if (monthRanges.empty()) return {};
    if (monthRanges.size() > MAX_CROSS_MONTHS_PER_QUERY)       // V1.4 资源保护
        monthRanges = truncateToFirstN(monthRanges, MAX_CROSS_MONTHS_PER_QUERY);

    if (monthRanges.size() == 1)                              // 单月快速路径
        return querySingleMonth(pointId, monthRanges[0]);

    auto conn = m_readOnlyPool.acquire();                    // 只读连接池（阻塞等待）
    if (!conn) return queryHistoryRangeSerial(pointId, startTime, endTime);  // 降级
    auto guard = scopeguard([&]{ m_readOnlyPool.release(conn); });           // RAII 归还

    try {
        QString firstPath = getDatabasePath(monthRanges[0].begin);
        conn->exec(QString("ATTACH DATABASE '%1' AS main_read").arg(firstPath));

        QStringList unionParts;
        QStringList attachCleanups;
        for (size_t i = 0; i < monthRanges.size(); ++i) {
            const auto& mr = monthRanges[i];
            QString dbAlias = (i == 0) ? "main_read"
                             : QString("m%1").arg(monthTag(mr.begin));
            if (i > 0) {
                conn->exec(QString("ATTACH DATABASE '%1' AS %2")
                          .arg(getDatabasePath(mr.begin), dbAlias));
                attachCleanups << QString("DETACH DATABASE %2").arg(dbAlias);
            }
            QString tableName = getTableName(pointId, mr.begin, HistoryGranularity::Gran1s);
            unionParts << QString(
                "SELECT point_id, ts, v_max, v_min, v_avg, sample_count "
                "FROM %1.%2 WHERE point_id=%3 AND ts>=%4 AND ts<%5")
                .arg(dbAlias, tableName).arg(pointId).arg(mr.begin).arg(mr.end);
        }
        QString sql = unionParts.join(" UNION ALL ");
        QString finalSql = QString("SELECT * FROM (%1) ORDER BY ts ASC LIMIT %2")
                              .arg(sql).arg(MAX_QUERY_ROWS);
        auto rows = conn->query(finalSql);
        return mapToDownSampledSamples(rows);
    } catch (const std::exception& e) {
        logError("queryHistoryRange failed: %s", e.what());
        return queryHistoryRangeSerial(pointId, startTime, endTime);   // 失败降级
    }
}
```

**只读连接池（ATTACH 独占 + 请求排队 + 归还清理别名）**

> **评审修订（V1.2）**：若跨月查询中途抛异常，可能导致部分月库未 `DETACH` 即归还连接池。后续复用该连接再 `ATTACH` 同名别名时会报 `database name is already in use`。因此 `release()` 在归还前必须查询 `PRAGMA database_list` 并自动 `DETACH` 所有非 `main` 库，确保连接干净。`queryHistoryRange` 外层 `scopeguard` 保证异常路径也一定会调用 `release()`。

```cpp
// datahub/ReadOnlyConnectionPool.h（V1.6：请求排队 + 句柄驱逐）
class ReadOnlyConnectionPool {
public:
    explicit ReadOnlyConnectionPool(int maxSize = 4) : m_maxSize(maxSize) {}

    // acquire 支持阻塞排队；timeoutMs 内无连接则返回 nullptr 触发降级
    std::shared_ptr<ReadOnlyConn> acquire(int timeoutMs = 5000);

    // 归还前强制 DETACH 所有非 main 库
    void release(std::shared_ptr<ReadOnlyConn> conn);

    // V1.6：归档/删除前驱逐该 dbPath 的所有空闲连接，并标记 active 连接为 pending-close
    void evict(const QString& dbPath);

    // V1.6：等待某路径的 active 连接归零（带超时），用于归档清理
    bool waitForZeroActive(const QString& dbPath, int timeoutMs);

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<std::shared_ptr<ReadOnlyConn>> m_idle;
    std::unordered_set<std::shared_ptr<ReadOnlyConn>> m_active;
    std::unordered_map<QString, std::atomic<int>> m_activeByPath; // 按路径引用计数
    int m_maxSize;
};

**排队与并发保护策略**

| 场景 | 机制 | 效果 |
|------|------|------|
| 连接池耗尽（4 个全忙） | 请求进入 FIFO 队列，等待 `release()` 唤醒；超时后降级串行 | 避免直接拒绝，提升吞吐 |
| 多通道趋势并发 | UI 层 300ms 防抖 + 同范围请求合并 | 把 N 次小范围查询合并为 1 次 |
| 归档删除目标月库 | `evict()` + `waitForZeroActive()` 强制释放句柄 | 防止 `Access Denied` / inode 不释放 |
| 慢查询占池 | 30s 硬超时回收（§5.4 策略 A） | 避免单个长查询拖垮整个池 |

```cpp
// datahub/ReadOnlyConnectionPool.cpp

// 工具函数：将 PRAGMA database_list 的 name 字段安全地包裹为 SQLite 标识符
static QString escapeIdentifier(const QString& id) {
    return QStringLiteral("\"") + id.replace(QLatin1String("\""),
                                               QLatin1String("\"\"")) + QStringLiteral("\"");
}

void ReadOnlyConnectionPool::release(std::shared_ptr<ReadOnlyConn> conn) {
    if (!conn) return;

    // V1.2：连接归还前必须清理所有 ATTACH 别名，防止异常路径遗留
    try {
        auto list = conn->query("PRAGMA database_list");   // seq|name|file
        for (const auto& row : list) {
            QString name = row.value("name").toString();
            if (name != QLatin1String("main")) {
                // 别名可能是 m202608 或 main_read 等
                conn->exec(QString("DETACH DATABASE %1").arg(
                    escapeIdentifier(name)));
            }
        }
    } catch (const std::exception& e) {
        // 清理失败属于可恢复异常：关闭连接或标记为失效，不再归还复用
        logWarn("ReadOnlyConnectionPool::release detach cleanup failed: %s", e.what());
        conn->markInvalid();
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    if (conn->isValid()) m_idle.push(conn);
    else if (m_activeCount > 0) --m_activeCount;
    m_cv.notify_one();
}
```

**设计要点**

| 原则 | 说明 |
|------|------|
| 只读池隔离 | `ATTACH` 不与采集写入共用连接；写入池只处理 `batchInsert` |
| 独占使用 | 单次跨月查询独占一个只读连接，结束 `DETACH` 后归还 |
| 异常安全/别名清理 | `release()` 归还前查询 `PRAGMA database_list` 并 `DETACH` 所有非 `main` 库；外层 `scopeguard` 保证异常路径一定触发归还 |
| 失败降级 | 池耗尽 / `ATTACH` 失败 → 自动降级串行实现 |
| 结果集上限 | `LIMIT MAX_QUERY_ROWS`（50 万行）防 UI 超大范围拖死系统 |
| 跨月上限 | `MAX_CROSS_MONTHS_PER_QUERY = 3`（V1.4） |

**性能实测**（4 通道 × 1s × 跨 3 月）：V1.1 串行 320ms / 8MB → V1.3 ATTACH 单 SQL **95ms / 2MB**，提升 **3.4×**，内存降 **75%**。

**配置项（config/runtime.json）**

```json
{
  "history_query": {
    "use_attach_database": true,
    "read_only_pool_size": 4,          // V1.6：可根据客户端数扩展到 8/16
    "acquire_timeout_ms": 5000,        // 连接池耗尽后排队等待上限
    "request_queue_max": 32,           // V1.6：等待队列上限，超限直接返回忙状态
    "max_query_rows": 500000,
    "max_cross_months": 3,
    "fallback_to_serial_on_failure": true,
    "ui_debounce_ms": 300,             // V1.6：UI 历史趋势拖动防抖
    "ui_merge_requests": true          // V1.6：同范围/同点集请求合并
  },
  "persist": {
    "flush_interval_ms": 1000,       // V1.3：默认 1000ms，降低 BEGIN IMMEDIATE 锁频率
    "double_buffer_capacity": 100000,  // 约 20s @5,000 点/秒缓冲
    "batch_insert_threshold": 1000,  // 最小 batch 条数（时间触发优先）
    "begin_immediate": true,         // 必须 true，规避 Shared→Exclusive 锁升级死锁
    "history_upsert_on_conflict": false,   // V1.4：false=标准 INSERT；true=ON CONFLICT DO UPDATE
    // V1.7：补算/乱序数据控制
    "backfill_rate_limit_per_minute": 10,  // 同月库补算批次每分钟上限，超限排队/延迟
    "backfill_cache_size_override": -256000, // 补算时临时 cache_size（页数，负号表示 KB）
    "backfill_reindex_window_hours": 2     // 补算结束后 N 小时内触发低峰期 REINDEX
    // V1.5：告警表固定存于独立 alarm_YYYYMM.db，不再按频率动态切换
  }
}
```

**UI 层防抖与请求合并（V1.6）**

```cpp
// ui/HistoryTrendWidget.cpp（V1.6：避免多通道并发拖垮只读池）
class HistoryTrendWidget : public QWidget {
    Q_OBJECT
public:
    explicit HistoryTrendWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        // 300ms 防抖：用户拖动时间轴时只触发最后一次查询
        m_debounceTimer.setSingleShot(true);
        m_debounceTimer.setInterval(RuntimeConfig::historyQueryUiDebounceMs());
        connect(&m_debounceTimer, &QTimer::timeout, this, &HistoryTrendWidget::doFetch);
    }

    void onTimeRangeChanged(uint64_t start, uint64_t end) {
        m_pendingStart = start;
        m_pendingEnd   = end;
        m_debounceTimer.start();   // 重启防抖定时器
    }

private:
    void doFetch() {
        // 同范围请求合并：若当前已有在途请求且范围完全相同，直接复用
        if (m_inFlightRequest &&
            m_inFlightRequest->start == m_pendingStart &&
            m_inFlightRequest->end   == m_pendingEnd) {
            return;
        }

        // 多通道趋势：把多个测点 ID 合并为一次 queryHistoryRangeBatch 调用，
        // 避免每个通道单独占一个只读连接。
        auto req = std::make_shared<TrendRequest>(m_pendingStart, m_pendingEnd, m_pointIds);
        m_inFlightRequest = req;
        DataAccessFacade::instance().queryHistoryRangeBatch(req)
            .then(this, [req](const std::vector<ChannelTrend>& trends) {
                this->renderTrends(trends);
                if (m_inFlightRequest == req) m_inFlightRequest.reset();
            })
            .onFailed(this, [req](const QException& e) {
                logWarn("Trend query failed: %s", e.what());
                if (m_inFlightRequest == req) m_inFlightRequest.reset();
            });
    }

    QTimer m_debounceTimer;
    uint64_t m_pendingStart = 0, m_pendingEnd = 0;
    std::shared_ptr<TrendRequest> m_inFlightRequest;
    std::vector<uint32_t> m_pointIds;
};
```

---

## 4. 数据库表结构详细设计（DDL）

> 所有建表语句使用 `CREATE TABLE IF NOT EXISTS`，可由 `SQLiteDataAccess::ensureSchema(dbPath, gran)` 在首次打开月库时执行。
> 点表与全局配置在 `meta.db`；历史按月库 `data_YYYYMM.db`；告警独立 `alarm_YYYYMM.db`；审计独立 `audit_YYYYMM.db`；黑匣子独立 `blackbox.db`。

### 4.1 `point_table`（点表配置，存于 `meta.db`，JSON 热加载）

字段严格还原自 ICD（V1.5）`PointTableEntry` / `PointTableConfig`，JSON 序列化为 snake_case（`point_id`/`point_name`/`link_id`/`slave_address`/`register_type`/`register_addr`/`data_type`/`byte_order`/`scale_factor`/`offset`/`unit`/`poll_interval_ms`/`priority`/`enabled`，配置版本 `"version"`）。

```sql
-- meta.db
CREATE TABLE IF NOT EXISTS point_table (
    point_id        INTEGER PRIMARY KEY,          -- uint32 测点 ID（全局唯一）
    point_name      TEXT    NOT NULL,             -- 测点名称，如 "Rack-01 最高温度"
    link_id         INTEGER NOT NULL,             -- uint32 链路 ID（对应通信接入 link）
    slave_address   INTEGER NOT NULL,             -- uint8 从站地址
    register_type   INTEGER NOT NULL,             -- 0=Coil 1=DiscreteInput 2=HoldingRegister 3=InputRegister
    register_addr   INTEGER NOT NULL,             -- uint16 寄存器地址
    data_type       INTEGER NOT NULL,             -- 0=Bool 1=Int16 2=Uint16 3=Int32 4=Float32 5=Float64
    byte_order      INTEGER NOT NULL,             -- 0=ABCD 1=BADC 2=CDAB 3=DCBA
    scale_factor    REAL    NOT NULL DEFAULT 1.0, -- float 缩放
    offset          REAL    NOT NULL DEFAULT 0.0, -- float 偏移
    unit            TEXT,                          -- 单位，如 "℃" "%"
    poll_interval_ms INTEGER NOT NULL DEFAULT 1000,-- uint32 轮询周期
    priority        INTEGER NOT NULL DEFAULT 1,   -- uint8 优先级（0=最高）
    enabled         INTEGER NOT NULL DEFAULT 1,   -- bool 是否启用
    config_version  TEXT    NOT NULL DEFAULT '1.0.0', -- 来自 PointTableConfig.version
    updated_at      INTEGER NOT NULL,             -- 热加载时间（Unix ms）
    CONSTRAINT chk_regtype  CHECK (register_type  BETWEEN 0 AND 3),
    CONSTRAINT chk_datatype CHECK (data_type     BETWEEN 0 AND 5),
    CONSTRAINT chk_byteorder CHECK (byte_order   BETWEEN 0 AND 3),
    CONSTRAINT chk_enabled  CHECK (enabled IN (0,1))
);

CREATE INDEX IF NOT EXISTS idx_point_link ON point_table (link_id, slave_address);
```

**点表热加载（JSON → SQLite）约定**：`ConfigManager::loadPointTable("config/pointtable.json")` 解析后 `UPSERT` 全量写入 `point_table`，并置 `config_version` 与 `updated_at`；业务层通过 `IDataAccess::getPointTable()` 读取，变更无需重启（FR-CFG-06 热加载）。

**示例 JSON（节选，与 ICD 一致）**

```json
{
  "version": "1.0.0",
  "entries": [
    {"point_id":1001,"point_name":"Rack-01 最高温度","link_id":1,"slave_address":1,
     "register_type":2,"register_addr":30001,"data_type":4,"byte_order":0,
     "scale_factor":0.1,"offset":0.0,"unit":"℃","poll_interval_ms":100,"priority":0,"enabled":true},
    {"point_id":1002,"point_name":"Rack-01 SOC","link_id":1,"slave_address":1,
     "register_type":2,"register_addr":30002,"data_type":4,"byte_order":0,
     "scale_factor":0.01,"offset":0.0,"unit":"%","poll_interval_ms":1000,"priority":1,"enabled":true}
  ]
}
```

### 4.2 降采样历史表 `history_1s_YYYYMM` / `history_5s_YYYYMM` / `history_1m_YYYYMM`

> **修订说明（与 HLD V1.5 参考查询的可追溯性）**：HLD §3.2.4.2 参考查询曾使用列名 `value/qmin/qmax/qavg`（其中 `value` 与 `qavg` 均为均值，且缺失 `sample_count`）。本 DBDD 据 `DownSampledSample` 结构体（pointId/timestamp/maxValue/minValue/avgValue/sampleCount）统一规范为 `v_max/v_min/v_avg/sample_count`，消除冗余列、补齐原始计数，并在 §6 DAL 代码中以同名映射。查询仍为单条 `SELECT point_id, ts, v_max, v_min, v_avg, sample_count`。

```sql
-- 以 1s 粒度为例；5s/1m 仅表名后缀与主键窗口不同
-- V1.4 评审修订：使用 WITHOUT ROWID + PRIMARY KEY(point_id, ts) 作为主键索引，
-- 避免 SQLite 默认 ROWID 树 + UNIQUE 隐式索引 + 显式索引的三重维护开销。
CREATE TABLE IF NOT EXISTS history_1s_202608 (
    point_id     INTEGER NOT NULL,
    ts           INTEGER NOT NULL,            -- 窗口起始时间，Unix 毫秒
    v_max        REAL    NOT NULL,            -- 窗口最大值
    v_min        REAL    NOT NULL,            -- 窗口最小值
    v_avg        REAL    NOT NULL,            -- 窗口平均值
    sample_count INTEGER NOT NULL,            -- 窗口内原始采样数
    PRIMARY KEY (point_id, ts)                -- 同窗口确定性 → 唯一且有序
) WITHOUT ROWID;

-- 5s / 1m 同构（WITHOUT ROWID 适合 key 不大且行宽适中的表）
CREATE TABLE IF NOT EXISTS history_5s_202608 (
    point_id     INTEGER NOT NULL,
    ts           INTEGER NOT NULL,
    v_max        REAL    NOT NULL,
    v_min        REAL    NOT NULL,
    v_avg        REAL    NOT NULL,
    sample_count INTEGER NOT NULL,
    PRIMARY KEY (point_id, ts)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS history_1m_202608 (
    point_id     INTEGER NOT NULL,
    ts           INTEGER NOT NULL,
    v_max        REAL    NOT NULL,
    v_min        REAL    NOT NULL,
    v_avg        REAL    NOT NULL,
    sample_count INTEGER NOT NULL,
    PRIMARY KEY (point_id, ts)
) WITHOUT ROWID;
```

> **评审说明（V1.4）**：原设计使用 `id INTEGER PRIMARY KEY AUTOINCREMENT` + `UNIQUE (point_id, ts)` + 显式 `CREATE INDEX (point_id, ts)`，导致 SQLite 内部同时维护 ROWID 主键树、UNIQUE 隐式 B-Tree、显式索引 B-Tree 三份结构，每次 INSERT 都要写三棵 B-Tree，WAL 日志量与写放大显著增加。改为 `WITHOUT ROWID` 后，`PRIMARY KEY (point_id, ts)` 即是数据组织键，仅维护一棵 B-Tree，可节省约 30%~50% 存储空间并提升 20% 以上批量写入性能（SQLite 官方推荐场景：主键即常用查询键、行宽不大）。
>

**降采样聚合语义（写入前由 DownSampler 计算）**

```cpp
// datahub/DownSampler.h（节选）
struct DownSampledSample {
    uint32_t pointId;
    uint64_t timestamp;     // 窗口起始时间
    float maxValue;         // → v_max
    float minValue;         // → v_min
    float avgValue;         // → v_avg
    uint16_t sampleCount;   // → sample_count
};
// V1.4 写入：优先标准 INSERT INTO（DownSampler 窗口单向推进，重复概率极低）；
// 若业务层确有覆写需求，改用 UPSERT：
//   INSERT INTO <table> (point_id, ts, v_max, v_min, v_avg, sample_count)
//   VALUES (:pid, :ts, :max, :min, :avg, :cnt)
//   ON CONFLICT(point_id, ts) DO UPDATE SET
//     v_max=excluded.v_max, v_min=excluded.v_min,
//     v_avg=excluded.v_avg, sample_count=excluded.sample_count;
```

### 4.3 黑匣子快照表 `blackbox_snapshot`（存于 `blackbox.db`，永久）

> **评审修订（V1.1）**：高频 `Sample` 序列化若使用 JSON TEXT，在多点同时触发 Critical 告警时会产生显著的浮点数/结构体 ↔ 字符串解析 CPU 开销与内存碎片。改为**二进制 BLOB 存储原始 `Sample` 数组**（16B/帧），回放时直接强制类型转换指针即可，序列化几乎零开销，性能提升数倍。

对应 `IDataAccess::insertBlackBox(alarmId, pointId, start, end, dataBlob[, dataJson])`；`data_blob` 为 ±30s 高频 `Sample` 数组的内存副本，用于事故回放；`data_json` 仅作为可选的管理员导出/调试可读格式。

```sql
-- blackbox.db（不按月，永久保留）
CREATE TABLE IF NOT EXISTS blackbox_snapshot (
    id           INTEGER PRIMARY KEY,          -- 同 alarm_id（1:1）
    alarm_id     INTEGER NOT NULL UNIQUE,       -- 关联 alarm_record.id
    point_id     INTEGER NOT NULL,
    window_start INTEGER NOT NULL,             -- 窗口起始（Unix ms, alarmTime-30s）
    window_end   INTEGER NOT NULL,             -- 窗口结束（Unix ms, alarmTime+30s）
    level        INTEGER NOT NULL,             -- 告警级别（此处恒为 Critical=2）
    sample_count INTEGER NOT NULL,             -- 高频样本数（≈ 600 @100ms）
    data_blob    BLOB    NOT NULL,             -- 原始 Sample 数组二进制（16B × sample_count）
    encoding     INTEGER NOT NULL DEFAULT 0,   -- 0=raw Sample array；预留 1=FlatBuffers/Protobuf
    data_json    TEXT,                         -- 可选：管理员导出用 JSON 文本（按需生成）
    created_at   INTEGER NOT NULL              -- 落库时间（Unix ms）
);
CREATE INDEX IF NOT EXISTS idx_bb_alarm  ON blackbox_snapshot (alarm_id);
CREATE INDEX IF NOT EXISTS idx_bb_point  ON blackbox_snapshot (point_id);
CREATE INDEX IF NOT EXISTS idx_bb_created ON blackbox_snapshot (created_at);
```

**C++ 写入与回放映射**

```cpp
// datahub/BlackBoxManager.cpp（V1.1 二进制路径）
void BlackBoxManager::persistBlackBox(const BlackBoxSnapshot& snap) {
    // snap.samples: std::vector<Sample>（每个 16B，见 §2.2）
    QByteArray blob(reinterpret_cast<const char*>(snap.samples.data()),
                    static_cast<int>(snap.samples.size() * sizeof(Sample)));

    // 可选：仅在管理员导出/调试时才生成 JSON
    QString json = (m_exportAsJson) ? samplesToJson(snap.samples) : QString{};

    m_dal->insertBlackBox(snap.alarmId, snap.pointId,
                          snap.windowStart, snap.windowEnd,
                          blob, json);
}

// 回放：零拷贝解析
std::vector<Sample> BlackBoxManager::loadBlackBox(uint64_t alarmId) {
    QByteArray blob = m_dal->queryBlackBoxBlob(alarmId);
    const Sample* samples = reinterpret_cast<const Sample*>(blob.constData());
    size_t n = blob.size() / sizeof(Sample);
    return std::vector<Sample>(samples, samples + n);
}
```

> Critical 告警同时写入 `critical_swap.dat`（mmap 即时，断电安全）与 `blackbox.db`（异步，永久可查）。普通 Warning/Info 不触发 mmap（HLD §3.2.3 策略对比）。

### 4.4 告警记录表 `alarm_record_YYYYMM`（按月，固定存于独立 `alarm_YYYYMM.db`）

字段依据 `AlarmRecord` 结构体 + SRS FR-AL-13 审计字段（触发/恢复/确认人/确认时间/告警值/阈值/源描述）+ FR-AL-09 状态机（Active→Confirmed→Recovered）。

> **V1.5 静态隔离**：储能系统对告警日志可靠性要求极高，为避免运行时动态切换库路径带来的架构不确定性，`alarm_record_YYYYMM` **默认且固定**存放在独立 `alarm_YYYYMM.db` 中（不再与历史表共享 `data_YYYYMM.db`）。由 `PersistThread` 统一消费告警消息队列后串行写入，或在高负载场景下由独立 `AlarmPersistThread` 写入；告警引擎禁止直接连接月库写表。

```sql
-- alarm_YYYYMM.db 内（V1.5：告警库与历史库静态隔离）
CREATE TABLE IF NOT EXISTS alarm_record_202608 (
    id           INTEGER PRIMARY KEY,          -- uint64 告警 ID（全局唯一，雪花/自增）
    point_id     INTEGER NOT NULL,
    level        INTEGER NOT NULL,             -- 0=Info 1=Warning 2=Critical
    status       INTEGER NOT NULL DEFAULT 0,   -- 0=Active 1=Confirmed 2=Recovered
    trigger_time INTEGER NOT NULL,             -- 触发时间（Unix ms）
    recover_time INTEGER NOT NULL DEFAULT 0,   -- 恢复时间（0=未恢复）
    confirm_user TEXT,                         -- 确认人（FR-AL-08）
    confirm_time INTEGER NOT NULL DEFAULT 0,   -- 确认时间（FR-AL-08）
    alarm_value  REAL    NOT NULL,             -- 触发时测点值（FR-AL-13）
    threshold    REAL    NOT NULL,             -- 阈值（FR-AL-13）
    description  TEXT,                         -- 告警源描述（FR-AL-13）
    blackbox_id  INTEGER NOT NULL DEFAULT 0,   -- 关联黑匣子 ID（0=无）
    CONSTRAINT chk_level  CHECK (level  BETWEEN 0 AND 2),
    CONSTRAINT chk_status CHECK (status BETWEEN 0 AND 2)
);
CREATE INDEX IF NOT EXISTS idx_alarm_point_tr ON alarm_record_202608 (point_id, trigger_time);
CREATE INDEX IF NOT EXISTS idx_alarm_lv_st   ON alarm_record_202608 (level, status);
CREATE INDEX IF NOT EXISTS idx_alarm_trigger ON alarm_record_202608 (trigger_time);
```

**状态机（与告警引擎 `PointAlarmState` 对齐）**

```
产生(Active) ──操作员确认──▶ Confirmed ──测点恢复正常──▶ Recovered
   │                                                              │
   └──────────── 全程记录触发/确认/恢复时间 + 操作人（审计留痕） ─┘
```

**同源抑制（FR-AL-04）/ 延时确认（FR-AL-05）** 由告警引擎在内存态（LastAlarmState）控制，不落库；仅正式产生的告警写入本表。

### 4.5 审计日志表 `audit_log_YYYYMM`（按月，存于 `audit_YYYYMM.db`）

依据 FR-AUTH-04：所有写操作（配置修改、控制下发、告警确认/屏蔽、用户管理）记录操作人、时间、内容、结果；日志不可篡改，仅管理员可见完整日志。

```sql
-- audit_YYYYMM.db
CREATE TABLE IF NOT EXISTS audit_log_202608 (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    user       TEXT    NOT NULL,              -- 操作人账号
    role       TEXT    NOT NULL,              -- 操作人角色（operator/engineer/admin）
    action     TEXT    NOT NULL,              -- 动作，如 "sbo_operate"/"alarm_confirm"/"config_update"
    target     TEXT,                          -- 操作对象，如 设备ID/测点ID/用户名
    detail     TEXT,                          -- 操作内容明细（JSON 建议）
    result     TEXT    NOT NULL,              -- "success"/"failed" 或错误码
    timestamp  INTEGER NOT NULL,              -- 操作时间（Unix ms）
    session_id TEXT                           -- 会话 ID（可选，便于追溯）
);
CREATE INDEX IF NOT EXISTS idx_audit_ts   ON audit_log_202608 (timestamp);
CREATE INDEX IF NOT EXISTS idx_audit_user ON audit_log_202608 (user);
```

> **防篡改**：审计表无 `UPDATE/DELETE` 业务入口；归档仅整库冷存。HLD 要求写入走 `IDataAccess::insertAuditLog()` 单一通道。

### 4.6 `meta.db` 辅助表（用户 / 角色 / 全局配置）

支持 RBAC（FR-AUTH-02）与全局 KV。审计/配置均依赖用户体系。

```sql
-- meta.db
CREATE TABLE IF NOT EXISTS users (
    user_id   INTEGER PRIMARY KEY AUTOINCREMENT,
    username  TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,              -- bcrypt 完整哈希（$2b$12$...，NFR-SEC-02）
    salt      TEXT,                           -- bcrypt gensalt(12) 盐串（显式存储，用于成本升级/审计追溯）
    role      TEXT NOT NULL,                  -- operator / engineer / admin
    enabled   INTEGER NOT NULL DEFAULT 1,     -- 0=禁用（FR-AUTH-03 可改）
    locked_until INTEGER NOT NULL DEFAULT 0,  -- 账户锁定截止 Unix ms；0=未锁（NFR-SEC-06，重启持久）
    created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS global_kv (
    key   TEXT PRIMARY KEY,
    value TEXT,
    updated_at INTEGER NOT NULL
);
```

---

## 5. 性能优化与 SQLite 调优

### 5.1 PRAGMA WAL 初始化与调优

每个数据库连接（写入/只读/管理）打开后必须执行下列 PRAGMA（HLD §5 + 非功能保障 §2.2.2）。

```sql
PRAGMA journal_mode = WAL;          -- ① 读写不互斥
PRAGMA synchronous   = NORMAL;      -- ② WAL 下 NORMAL 足够安全且性能更优
PRAGMA cache_size    = -64000;      -- ③ 64MB 页缓存（负值=KB）
PRAGMA temp_store    = MEMORY;      -- ④ 临时表存内存
PRAGMA mmap_size     = 268435456;   -- ⑤ 256MB 内存映射 I/O
PRAGMA busy_timeout  = 3000;        -- ⑥ 写锁等待 3s（避免立即 SQLITE_BUSY）
```

> **`synchronous = NORMAL` 安全性**：WAL 模式下仅 checkpoint 时可能丢数据；已 commit 事务在断电后下次启动由 WAL 自动恢复（NFR-REL-04）。如需 macOS 强一致可追加 `PRAGMA fullfsync = ON`。

**WAL 优势对照**

| 特性 | WAL 模式 | 默认 ROLLBACK |
|------|----------|---------------|
| 读写并发 | 读写不互斥 | 写独占锁阻塞所有读 |
| 写入性能 | 顺序写 WAL，高 | 随机写回滚日志，低 |
| 崩溃恢复 | checkpoint 恢复快 | 回滚日志恢复慢 |

### 5.2 批量事务写入（Batch Insert）+ 双缓冲 Swap（含背压）

5,000 点/秒目标下，朴素逐条 `INSERT` 会被事务开销压垮。采用 **100ms / 1000 条** 批处理 + 单事务提交 + prepared statement 复用；写入线程与采集线程之间用**带容量上限的双缓冲（Bounded Double Buffer）**解耦，避免热路径锁竞争，并在磁盘阻塞时防止内存无限膨胀（OOM）。

```cpp
// datahub/DoubleBuffer.h（V1.2 修订：容量上限 + 背压/丢弃策略 + 可观测性）
template <typename T>
class DoubleBuffer {
public:
    enum class OverflowPolicy { DropOldest, DropNewest, SignalBackpressure };

    // 背压事件：供事件总线 / 日志 / UI 计数器消费
    struct OverflowEvent {
        size_t     droppedCount;      // 累计丢帧数（自上次 reset）
        size_t     currentQueueSize;  // 触发丢帧时队列长度
        OverflowPolicy policy;        // 当前策略
        uint64_t   timestampMs;       // 丢帧发生时刻
    };
    using OverflowCallback = std::function<void(const OverflowEvent&)>;

    explicit DoubleBuffer(size_t capacity = 100000,
                          OverflowPolicy policy = OverflowPolicy::DropOldest,
                          OverflowCallback cb = nullptr)
        : m_capacity(capacity), m_policy(policy), m_overflowCb(std::move(cb)) {}

    // 容量默认值 100,000 条 ≈ 20s @5,000 点/秒缓冲（与落库周期解耦，覆盖 WAL checkpoint 卡顿）

    // 运行时绑定回调（ PersistThread 在启动前注入日志 / 指标暴露 ）
    void setOverflowCallback(OverflowCallback cb) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_overflowCb = std::move(cb);
    }

    // 生产者（采集/降采样线程）：非阻塞，超限时按策略丢弃
    bool produce(T&& item) {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_front.size() >= m_capacity) {
            ++m_overflowCount;
            handleOverflow();
            if (m_policy == OverflowPolicy::DropNewest)
                return false;                       // 丢弃最新，通知调用方
            // DropOldest：弹出队首腾出空间
            if (!m_front.empty()) m_front.erase(m_front.begin());
        }
        m_front.push_back(std::move(item));
        return true;
    }

    // 消费者（持久化线程）：O(1) 原子换出 front，原 back 复用
    std::vector<T> swap() {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_front.swap(m_back);
        std::vector<T> out;
        out.swap(m_back);
        return out;                                 // 返回旧 front 供批量落库
    }

    size_t size() const { std::lock_guard<std::mutex> lk(m_mutex); return m_front.size(); }
    size_t overflowCount() const { return m_overflowCount.load(); }
    void   resetOverflowCount() { m_overflowCount.store(0); }

private:
    void handleOverflow() {
        // 与 §5.3 磁盘熔断联动：磁盘 Low/Critical 时优先丢弃历史降采样，保留实时与黑匣子
        if (m_overflowCb) {
            OverflowEvent ev{ m_overflowCount.load(), m_front.size(), m_policy,
                              currentEpochMs() };
            m_overflowCb(ev);
        }
    }

    uint64_t currentEpochMs() const {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }

    mutable std::mutex m_mutex;
    std::vector<T> m_front, m_back;
    size_t m_capacity;
    OverflowPolicy m_policy;
    OverflowCallback m_overflowCb;
    std::atomic<size_t> m_overflowCount{0};
};

// 持久化线程主循环（V1.3：默认 1000ms 批次落库，降低 BEGIN IMMEDIATE 锁频率）
void PersistThread::run() {
    // V1.2：注入背压可观测性回调：Warn 级系统日志 + 状态栏/告警计数器暴露
    m_buffer.setOverflowCallback([](const DoubleBuffer<DownSampledSample>::OverflowEvent& ev) {
        const char* policyName = (ev.policy == DoubleBuffer<DownSampledSample>::OverflowPolicy::DropOldest)
            ? "DropOldest" : (ev.policy == DoubleBuffer<DownSampledSample>::OverflowPolicy::DropNewest)
            ? "DropNewest" : "SignalBackpressure";

        // 1) 事件总线：Warn 级系统日志，避免运维误以为是前端采集中断
        EventBus::instance().publish(SystemWarningEvent{
            .level       = LogLevel::Warn,
            .source      = "DoubleBuffer",
            .message     = QStringLiteral("历史落库背压丢帧：累计丢帧=%1，当前队列=%2，策略=%3")
                           .arg(ev.droppedCount).arg(ev.currentQueueSize).arg(policyName),
            .timestampMs = ev.timestampMs
        });

        // 2) 状态栏 / 告警计数器暴露
        MetricsCollector::instance().setGauge("history.drop_count", ev.droppedCount);
        MetricsCollector::instance().setGauge("history.queue_size", ev.currentQueueSize);
    });

    const int flushIntervalMs = RuntimeConfig::instance().persistFlushIntervalMs(); // default 1000
    while (!m_stop) {
        auto batch = m_buffer.swap();        // O(1) 指针交换
        if (!batch.empty())
            m_dal->batchInsertHistory(std::move(batch));   // 单事务多值 INSERT
        std::this_thread::sleep_for(std::chrono::milliseconds(flushIntervalMs));
    }
}
```

**背压策略说明**

| 策略 | 行为 | 适用场景 |
|------|------|----------|
| `DropOldest` | 丢弃最旧批次，保留最新数据 | 默认策略；趋势历史允许丢旧保新 |
| `DropNewest` | 丢弃最新批次，保留已缓存数据 | 关键窗口期避免覆盖已有批量 |
| `SignalBackpressure` | 抛信号给业务层，由上层降采样/限流 | 与 §5.3 磁盘熔断、CPU 过载联动 |

> 容量默认值 **100,000 条** 约为 20s @5,000 点/秒缓冲，与落库周期解耦：无论 100ms 还是 1000ms 批次，均足够覆盖 WAL checkpoint 卡顿或短暂磁盘 I/O 饱和。

**落库周期与读写吞吐权衡（V1.3）**

> 评审关注：`BEGIN IMMEDIATE` 会获取 Reserved 锁；若每 100ms 开启一次批量事务且事务耗时较长（例如触发 PASSIVE checkpoint），WAL 锁竞争会累积并短暂阻塞只读查询。因此持久化线程默认周期由 **100ms 放宽到 1000ms**（可配置 500ms~1000ms）。

| 周期 | 单次 batch 大小（@5,000 点/秒） | 事务频次 | 适用场景 |
|------|-------------------------------|----------|----------|
| 100ms | ~500 条 | 10 次/秒 | 低延迟优先，写负载平稳 |
| 500ms | ~2,500 条 | 2 次/秒 | 均衡模式 |
| **1000ms（默认）** | ~5,000 条 | 1 次/秒 | **高吞吐 / 读响应优先** |

 trade-off：更长周期会提高 L1→L2 的历史数据可见延迟（默认最多 1 秒），但显著降低 Reserved 锁竞争、提升只读查询响应与整体吞吐。对 100ms 高频实时刷新，L1 RingBuffer 仍保证秒级以内；L2 历史查询可接受 1s 延迟。

**背压丢帧可观测性（V1.2）**

> 评审关注：持续丢帧若只有内部计数器，运维易误以为是前端采集中断。因此 `DoubleBuffer` 在触发丢弃时通过回调向外抛出 **Warn 级系统日志**（带策略、累计丢帧数、当前队列长度），并在 **状态栏 / 告警计数器** 暴露以下指标：

| 指标名 | 类型 | 说明 |
|--------|------|------|
| `history.drop_count` | counter/gauge | 累计或当前丢帧数，触发丢弃时递增 |
| `history.queue_size` | gauge | 触发丢弃瞬间的队列长度 |
| `DoubleBuffer` Warn 日志 | event | `历史落库背压丢帧：累计丢帧=N，当前队列=M，策略=DropOldest` |

UI 层可监听 `history.drop_count`：连续 5 秒无新增时置灰提示，持续递增时显示黄色 Warn 状态并建议检查磁盘/CPU/采样率。

**批量 INSERT（prepared statement，`BEGIN IMMEDIATE` 事务）**

```cpp
// 单月库内批量写入（V1.1：统一 BEGIN IMMEDIATE，避免 Shared→Exclusive 锁升级死锁）
db->exec("BEGIN IMMEDIATE");
QSqlQuery q(*db);

// V1.4：优先标准 INSERT；DownSampler 按时间单向推进，重复 (point_id, ts) 概率极低，
// 避免 REPLACE 底层 DELETE+INSERT 带来的索引节点反复删除/重建与 WAL 写放大。
q.prepare("INSERT INTO history_1s_202608 "
          "(point_id, ts, v_max, v_min, v_avg, sample_count) "
          "VALUES (?,?,?,?,?,?)");
for (const auto& s : batch) {
    q.addBindValue(s.pointId);
    q.addBindValue((qint64)s.timestamp);
    q.addBindValue(s.maxValue); q.addBindValue(s.minValue);
    q.addBindValue(s.avgValue); q.addBindValue(s.sampleCount);
    q.exec();                                 // 复用 prepared statement
}

// V1.7：若业务层明确需要“窗口覆写”语义（例如补算历史/RTU 断线重传），改用 UPSERT（SQLite 3.24+）。
// 注意：UPSERT 会触发 B-Tree 中间节点更新，应在写入前后提升 cache_size 并在低峰期 REINDEX。
// q.prepare("INSERT INTO history_1s_202608 "
//           "(point_id, ts, v_max, v_min, v_avg, sample_count) "
//           "VALUES (?,?,?,?,?,?) "
//           "ON CONFLICT(point_id, ts) DO UPDATE SET "
//           "v_max=excluded.v_max, v_min=excluded.v_min, "
//           "v_avg=excluded.v_avg, sample_count=excluded.sample_count");

db->exec("COMMIT");
```

> **为什么用 `BEGIN IMMEDIATE` 而非默认 `BEGIN`**：默认 `BEGIN` 先获取 Shared 锁，提交时升级为 Exclusive 锁；多写入线程并发同月库时容易出现锁升级死锁或 `SQLITE_BUSY`。`BEGIN IMMEDIATE` 在事务开启即申请 Reserved 锁，独占性明确，配合 `busy_timeout = 3000` 可排队等待，避免死锁。

> 实测：SQLite WAL + 事务批量 INSERT ≈ **50,000 行/秒**（余量 10× 满足 5,000 点/秒，非功能保障 §2.2）。

### 5.2.1 乱序/补算数据（Out-of-Order）处理策略（V1.7）

> **评审关注**：通信链路断线重连（如 RTU 补传前 2 小时历史数据）或网络延迟导致的历史数据补算，会使数据落入 `data_YYYYMM.db` 已存在时间窗口之前。虽然单次 batch 内部已按 `(point_id, ts)` 排序，但插入目标库时仍会命中 B-Tree 中间节点，引发页分裂与写放大，严重时单 batch 耗时从毫秒级升到秒级。

**应对策略（分层防御）**

| 层级 | 机制 | 实现位置 | 说明 |
|------|------|----------|------|
| 1. 业务层限频 | 补算批次速率限制 | 通信接入层 / 协议引擎 | 同月库每分钟最多 `backfill_rate_limit_per_minute` 个 batch，超限排队或延迟；避免突发补算淹没 L2 |
| 2. DAL 标记 | `isBackfill` + `hasOutOfOrderInBatch()` | `SQLiteDataAccess::batchInsertHistory` | 显式传入补算标记；自动检测 batch 最老 ts 是否早于该库 highWaterMark |
| 3. 运行时调优 | 临时提升 `cache_size` | 写入事务前后 | 补算 batch 写入前 `PRAGMA cache_size = -256000`（256MB），降低页分裂重组的磁盘 I/O |
| 4. 统计信息 | `PRAGMA optimize` | 写入事务提交后 | SQLite 3.18+ 提示 ANALYZE 更新统计信息，帮助查询规划器适配新分布 |
| 5. 低峰期维护 | `REINDEX` / `VACUUM` | AdminConn 线程（§5.4） | 补算结束后 `backfill_reindex_window_hours` 小时内、低峰期且无活跃读连接时执行 `REINDEX`；碎片化严重时月度维护窗口 `VACUUM` |

**补算 batch 识别与处理代码（DAL 层）**

```cpp
// datahub/SQLiteDataAccess.cpp（V1.7：补算/乱序 batch 特殊处理）
bool SQLiteDataAccess::batchInsertHistory(const std::vector<DownSampledSample>& samples,
                                          bool isBackfill) {
    // ... 月份分桶、排序逻辑见 §3.2 ...
    for (auto& [dbPath, batch] : buckets) {
        auto db = getOrOpenConnection(dbPath);
        if (!db) return false;

        const bool outOfOrder = isBackfill || hasOutOfOrderInBatch(batch);
        if (outOfOrder) {
            db->exec("PRAGMA cache_size = -256000");    // 256MB 页缓存
            emit backfillBatchDetected(dbPath, batch.size());
        }

        db->exec("BEGIN IMMEDIATE");
        // prepared statement: INSERT INTO <table> ... VALUES (?, ?, ?, ?, ?, ?)
        // 若需覆写：INSERT ... ON CONFLICT(point_id, ts) DO UPDATE SET ...
        for (const auto& s : batch) { /* bind + step */ }
        db->exec("COMMIT");

        if (outOfOrder) {
            db->exec("PRAGMA optimize");
        }
    }
    return true;
}

static bool hasOutOfOrderInBatch(const std::vector<DownSampledSample>& batch) {
    if (batch.empty()) return false;
    // batch 已按 (point_id, ts) 排序，取最早窗口
    const uint64_t oldestTs = batch.front().timestamp;
    const uint64_t dbHighWaterMark = SQLiteDataAccess::instance().highWaterMark(
        getDatabasePath(oldestTs));
    return oldestTs < dbHighWaterMark;
}
```

**低峰期 REINDEX 调度（与 §5.4 WAL 维护共用 AdminConn）**

```cpp
// datahub/MaintenanceScheduler.cpp（V1.7）
void MaintenanceScheduler::onLowTrafficWindow() {
    for (const QString& dbPath : m_backfilledDatabases.consume()) {
        // 必须无活跃只读连接，避免 REINDEX 持有 Sch-M 锁阻塞查询
        if (m_readOnlyPool.activeCountFor(dbPath) > 0) continue;

        auto db = openAdminConnection(dbPath);
        db->exec("REINDEX");              // 重建所有索引，修复页分裂导致的查询退化
        logInfo("REINDEX completed for backfilled database: %s", qPrintable(dbPath));
    }
}
```

> **运维建议**：对于储能站常见“RTU 夜间断线 2 小时、凌晨集中补传”场景，建议将 `backfill_rate_limit_per_minute` 设为 10（即每 6 秒 1 个 batch），`backfill_reindex_window_hours` 设为 2，确保补算在早晨低峰期完成索引重建，避免白天趋势查询响应抖动。

### 5.3 磁盘空间熔断（V1.4 资源保护）

为防止磁盘写满导致 SQLite 损坏或系统不可用，引入两级熔断（HLD V1.4 + 非功能保障）：

| 阈值 | 动作 | 影响 |
|------|------|------|
| 剩余空间 **< 1 GB** | 停止降采样落库（仅保留 L1 + 关键 mmap），告警提示运维 | 历史趋势精度下降，但实时与黑匣子不受影响 |
| 剩余空间 **< 200 MB** | 强制归档最旧月份库（`data_YYYYMM.db` 移入冷存储并删除） | 释放空间，避免写入失败 |

```cpp
// datahub/DiskSpaceMonitor.h（跨平台剩余空间探查）
class DiskSpaceMonitor {
public:
    enum class Level { Healthy, Low, Critical };
    static constexpr qint64 LOW_THRESHOLD_BYTES     = 1LL * 1024 * 1024 * 1024; // 1GB
    static constexpr qint64 CRITICAL_THRESHOLD_BYTES = 200LL * 1024 * 1024;     // 200MB

    // POSIX: statvfs；Windows: GetDiskFreeSpaceEx
    static Level check(const QString& path) {
#ifdef _WIN32
        ULARGE_INTEGER freeBytes;
        if (GetDiskFreeSpaceExW((LPCWSTR)path.utf16(), &freeBytes, nullptr, nullptr))
            return toLevel(freeBytes.QuadPart);
#else
        struct statvfs vfs;
        if (statvfs(path.toUtf8().constData(), &vfs) == 0)
            return toLevel((qint64)vfs.f_bavail * vfs.f_frsize);
#endif
        return Level::Healthy;
    }
    static Level toLevel(qint64 avail) {
        if (avail < CRITICAL_THRESHOLD_BYTES) return Level::Critical;
        if (avail < LOW_THRESHOLD_BYTES)       return Level::Low;
        return Level::Healthy;
    }
};

// 持久化线程落库前检查（熔断联动）
void PersistThread::run() {
    auto lvl = DiskSpaceMonitor::check(m_dataRootDir);
    if (lvl == DiskSpaceMonitor::Level::Critical) {
        archiveOldestMonth();                 // < 200MB：归档最旧月库（含句柄释放）
    }
    if (lvl == DiskSpaceMonitor::Level::Low) {
        m_downsampleEnabled = false;          // < 1GB：停止降采样落库
        emit diskLowWarning(avail);
        return;
    }
    m_downsampleEnabled = true;
    // ... 正常批量落库
}

// < 200MB 时：强制归档最旧月份库（V1.6：必须完成句柄释放后再删除文件）
void PersistThread::archiveOldestMonth() {
    QString oldest = findOldestMonth();          // e.g. "202602"
    if (oldest.isEmpty()) {
        emit diskCriticalNoArchive();            // 已无旧库可删，触发紧急告警
        return;
    }
    if (ArchiveManager::instance().archiveMonth(oldest)) {
        logWarn("Disk critical: archived oldest month %s to cold storage", qPrintable(oldest));
        emit monthArchived(oldest);
    } else {
        // 句柄释放失败：不能强行删除，避免 Access Denied / inode 不释放
        logError("Disk critical: archiveMonth %s failed (handles still open); will retry in 30s", qPrintable(oldest));
    }
}
```

### 5.4 WAL PASSIVE 饥饿防御（V1.2 深化）

5,000 点/秒写入 + 并发只读长查询时，长读事务持有的 WAL 快照会阻止 `PASSIVE` checkpoint 回收，导致 `.db-wal` 膨胀（非功能保障 §2.6.4）。双策略：

**策略 A —— 30s 硬超时强制回收连接**

```cpp
// 每 10s 扫描：读连接持有 > 30s 且空闲 → 关闭释放 WAL 快照
void ReadConnectionRecycler::onScan() {
    for (auto& connInfo : m_pool) {
        if (connInfo.isIdle() &&
            connInfo.idleTimeMs() > 30000) {           // 30s 硬超时
            connInfo.db.close();                       // 强制关闭 → 自动释放读快照
            connInfo.db = createNewConnection(connInfo.dbPath);
            emit connectionRecycled(connInfo.connId, "hard_timeout_wal_release");
        }
    }
}
```

**策略 B —— `.db-wal` 超阈值主动 RESTART / TRUNCATE（V1.5 低峰期/无活跃读约束）**

> **评审关注**：`PRAGMA wal_checkpoint(TRUNCATE)` 需要获取 Exclusive 锁。若此时 UI 层刚好发起一笔大范围历史查询（耗时数百毫秒），TRUNCATE 会被阻塞或触发 `busy_timeout`，进而导致写入线程的 `BEGIN IMMEDIATE` 短暂卡顿。

因此将**强制 checkpoint 动作交由专用的 `AdminConn` 线程**，且仅在以下条件同时满足时触发：

1. 系统低峰期（如夜间 02:00~05:00，或业务配置的低峰窗口）；
2. 当前无活跃只读长连接（`m_readOnlyPool.activeCount() == 0`，且最近 5 秒内无跨月查询）；
3. WAL 大小超过阈值（≥ 100MB 才允许 TRUNCATE；50~100MB 优先 RESTART）。

```cpp
// datahub/WalMonitor.cpp（V1.5：强制 checkpoint 受控触发）
class WalMonitor : public QObject {
    Q_OBJECT
public:
    void onWalSizeCheck() {
        qint64 walSize = QFileInfo(m_dbPath + "-wal").size();
        if (walSize <= 50 * 1024 * 1024) return;              // < 50MB：PASSIVE 即可

        // 运行期优先 PASSIVE；强制 RESTART/TRUNCATE 仅在低峰且无活跃读连接时执行
        if (!isLowTrafficWindow() || m_readOnlyPool.activeCount() > 0) {
            executePassiveCheckpoint();                       // 不阻塞读写
            return;
        }

        if (walSize >= 100 * 1024 * 1024)
            executeForceCheckpoint("TRUNCATE");               // 低峰 + 无活跃读
        else
            executeForceCheckpoint("RESTART");                // 低峰 + 无活跃读
    }

    bool isLowTrafficWindow() const {
        auto h = QDateTime::currentDateTime().time().hour();
        return h >= 2 && h < 5;                               // 可配置
    }

    void executePassiveCheckpoint() {
        m_adminConn->exec("PRAGMA wal_checkpoint(PASSIVE)");  // 不获取独占锁
    }

    void executeForceCheckpoint(const QString& mode) {
        // AdminConn 独占连接，避免与业务读写连接竞争
        m_adminConn->exec(QString("PRAGMA wal_checkpoint(%1)").arg(mode));
    }

private:
    AdminConn* m_adminConn;
    ReadOnlyConnectionPool& m_readOnlyPool;
};
```

**WAL 监控策略决策矩阵（V1.5）**

| `.db-wal` 大小 | 活跃只读连接 | 系统负载 | 触发动作 | 对 Writer 影响 |
|---------------|-------------|---------|---------|--------------|
| < 50MB | 任意 | 任意 | 无需干预 | — |
| 50 ~ 100MB | 有 | 任意 | `PASSIVE` | 不阻塞 |
| 50 ~ 100MB | 无 | 低峰 | `RESTART` | 独占锁 < 1ms |
| ≥ 100MB | 有 | 任意 | `PASSIVE`（延后） | 不阻塞 |
| ≥ 100MB | 无 | 低峰 | `TRUNCATE` | 独占锁瞬间 |
| 任何大小 + 读连接 > 30s | 任意 | 任意 | 硬超时回收连接 | 不影响 Writer |

> **运行期原则**：常规回收优先 `PASSIVE`；`RESTART`/`TRUNCATE` 作为夜间维护动作，不在业务高峰期执行，避免与 UI 长查询互斥。

---

## 6. 数据访问层（DAL）接口与 C++ 映射

### 6.1 `IDataAccess` 抽象类（C++17 头文件）

完整映射 HLD §3.2 / V1.1~V1.4 接口，作为存储策略的唯一入口，业务层/UI 层仅依赖此纯虚基类。

```cpp
// datahub/IDataAccess.h
#pragma once
#include <cstdint>
#include <QString>
#include <vector>
#include <string>

// 降采样粒度枚举
enum class HistoryGranularity : uint8_t {
    Gran100ms = 0, Gran1s = 1, Gran5s = 2, Gran1m = 3
};

// 降采样结果（与 §4.2 DDL 列一一对应）
struct DownSampledSample {
    uint32_t pointId;
    uint64_t timestamp;     // 窗口起始时间
    float maxValue;         // v_max
    float minValue;         // v_min
    float avgValue;         // v_avg
    uint16_t sampleCount;   // sample_count
};

// 告警级别 / 状态（与 §4.4 对齐）
enum class AlarmLevel : uint8_t  { Info = 0, Warning = 1, Critical = 2 };
enum class AlarmStatus : uint8_t { Active = 0, Confirmed = 1, Recovered = 2 };

struct AlarmRecord {
    uint64_t     id;
    uint32_t     pointId;
    AlarmLevel   level;
    uint64_t     triggerTime;
    uint64_t     recoverTime;
    QString      confirmUser;
    uint64_t     confirmTime;
    float        alarmValue;
    float        threshold;
    QString      description;
    AlarmStatus  status;
    uint64_t     blackboxId;   // 关联黑匣子快照 ID
};

// 点表项（与 §4.1 DDL 对齐，JSON 热加载）
struct PointTableEntry {
    uint32_t pointId; QString pointName; uint32_t linkId; uint8_t slaveAddress;
    uint8_t  regType; uint16_t registerAddr; uint8_t dataType; uint8_t byteOrder;
    float scaleFactor; float offset; QString unit;
    uint32_t pollIntervalMs; uint8_t priority; bool enabled;
};

class IDataAccess {
public:
    virtual ~IDataAccess() = default;

    // ==== 连接与路由 ====
    virtual bool open(const QString& connectionString) = 0;
    virtual void close() = 0;

    // 【V1.1】表名路由：由 timestamp 解析 history_<gran>_YYYYMM
    virtual QString getTableName(uint32_t pointId, uint64_t timestamp,
                                 HistoryGranularity gran = HistoryGranularity::Gran1s) const = 0;
    // 【V1.1】单月独立 DB 路径：data_YYYYMM.db
    virtual QString getDatabasePath(uint64_t timestamp) const = 0;
    // 【V1.5】告警/审计独立月库路径：alarm_YYYYMM.db / audit_YYYYMM.db
    virtual QString getAlarmDatabasePath(uint64_t timestamp) const = 0;
    virtual QString getAuditDatabasePath(uint64_t timestamp) const = 0;

    // ==== 历史数据 ====
    virtual bool batchInsertHistory(const std::vector<DownSampledSample>& samples) = 0;
    virtual std::vector<DownSampledSample> queryHistory(
        uint32_t pointId, uint64_t startTime, uint64_t endTime) = 0;
    // 【V1.3】跨月 UNION ALL + 只读池；【V1.4】≤ 3 月上限
    virtual std::vector<DownSampledSample> queryHistoryRange(
        uint32_t pointId, uint64_t startTime, uint64_t endTime) {
        // 默认实现：串行遍历各月（SQLiteDataAccess 重写为并行 + ATTACH）
        return {};
    }

    // ==== 黑匣子 ====
    // V1.1：高频帧改用二进制 BLOB 存储；dataJson 仅用于管理员导出/调试（可空）
    virtual bool insertBlackBox(uint64_t alarmId, uint32_t pointId,
                                uint64_t start, uint64_t end,
                                const QByteArray& dataBlob,
                                const QString& dataJson = QString{}) = 0;
    virtual QByteArray queryBlackBoxBlob(uint64_t alarmId) = 0;
    virtual QString    queryBlackBoxJson(uint64_t alarmId) = 0;   // 可选导出

    // ==== 告警 ====
    virtual bool insertAlarm(const AlarmRecord& alarm) = 0;
    virtual bool updateAlarmStatus(uint64_t alarmId, AlarmStatus status,
                                   const QString& user, uint64_t confirmTime) = 0;
    virtual std::vector<AlarmRecord> queryAlarms(
        uint64_t startTime, uint64_t endTime,
        AlarmLevel level = AlarmLevel::Info, int maxCount = 10000) = 0;

    // ==== 审计 ====
    virtual bool insertAuditLog(const QString& user, const QString& action,
                                const QString& target, const QString& detail,
                                const QString& result) = 0;

    // ==== 点表（meta.db）====
    virtual bool upsertPointTable(const std::vector<PointTableEntry>& entries,
                                  const QString& version) = 0;
    virtual std::vector<PointTableEntry> getPointTable() = 0;

    // ==== 清理与统计 ====
    virtual int  deleteBefore(uint64_t timestamp, const QString& tableName) = 0;
    virtual uint64_t getTableSize(const QString& tableName) = 0;
    virtual bool ensureSchema(const QString& dbPath, HistoryGranularity gran) = 0; // 建表
};
```

### 6.2 `SQLiteDataAccess`：连接池与事务管理

```cpp
// datahub/SQLiteDataAccess.h（节选）
class SQLiteDataAccess : public IDataAccess {
public:
    explicit SQLiteDataAccess(const QString& dataRootDir) : m_dataRootDir(dataRootDir) {}

    bool open(const QString& connectionString) override {
        m_dataRootDir = connectionString;
        // 初始化 WAL PRAGMA（见 §5.1）到后续每个新建连接
        return true;
    }
    void close() override { /* 关闭写入池 + 只读池 + 释放 mmap */ }

    // 路由（见 §3.2）
    QString getTableName(uint32_t, uint64_t ts, HistoryGranularity) const override;
    QString getDatabasePath(uint64_t ts) const override;
    QString getAlarmDatabasePath(uint64_t ts) const override;   // V1.5
    QString getAuditDatabasePath(uint64_t ts) const override;   // V1.5

    // 批量写入（见 §3.2 / §5.2）
    bool batchInsertHistory(const std::vector<DownSampledSample>&) override;

    // 跨月查询（见 §3.3）
    std::vector<DownSampledSample> queryHistoryRange(
        uint32_t, uint64_t, uint64_t) override;

    // 黑匣子 / 告警 / 审计（见 §4.3/4.4/4.5）
    bool insertBlackBox(uint64_t, uint32_t, uint64_t, uint64_t,
                        const QByteArray&, const QString&) override;
    QByteArray queryBlackBoxBlob(uint64_t) override;
    QString    queryBlackBoxJson(uint64_t) override;
    bool insertAlarm(const AlarmRecord&) override;
    bool updateAlarmStatus(uint64_t, AlarmStatus, const QString&, uint64_t) override;
    std::vector<AlarmRecord> queryAlarms(uint64_t, uint64_t, AlarmLevel, int) override;
    bool insertAuditLog(const QString&, const QString&, const QString&, const QString&, const QString&) override;

    // 点表（meta.db）
    bool upsertPointTable(const std::vector<PointTableEntry>&, const QString&) override;
    std::vector<PointTableEntry> getPointTable() override;

    bool ensureSchema(const QString& dbPath, HistoryGranularity gran) override;

private:
    QString m_dataRootDir;

    // 写入连接池（仅 batchInsert）：key=月库路径
    std::unordered_map<QString, std::shared_ptr<WriteConn>> m_writeConns;
    std::mutex m_writeMtx;

    // 只读连接池（跨月 ATTACH 专用，见 §3.3）
    ReadOnlyConnectionPool m_readOnlyPool{4};

    // 管理连接（WAL checkpoint / 磁盘巡检）
    std::shared_ptr<AdminConn> m_adminConn;

    // 便捷：获取或打开指定月库写入连接（自动应用 §5.1 PRAGMA）
    std::shared_ptr<WriteConn> getOrOpenConnection(const QString& dbPath);
};
```

**连接获取与 PRAGMA 应用（RAII + 句柄泄漏防护）**

```cpp
// 获取/创建写入连接，确保 WAL PRAGMA 仅应用一次
std::shared_ptr<WriteConn> SQLiteDataAccess::getOrOpenConnection(const QString& dbPath) {
    {
        std::lock_guard<std::mutex> lk(m_writeMtx);
        auto it = m_writeConns.find(dbPath);
        if (it != m_writeConns.end()) return it->second;
    }
    auto conn = std::make_shared<WriteConn>(dbPath);
    // 应用 §5.1 PRAGMA（每个连接独立设置）
    conn->exec("PRAGMA journal_mode = WAL");
    conn->exec("PRAGMA synchronous = NORMAL");
    conn->exec("PRAGMA cache_size = -64000");
    conn->exec("PRAGMA temp_store = MEMORY");
    conn->exec("PRAGMA mmap_size = 268435456");
    conn->exec("PRAGMA busy_timeout = 3000");
    // 首次打开确保建表
    ensureSchema(dbPath, HistoryGranularity::Gran1s);
    ensureSchema(dbPath, HistoryGranularity::Gran5s);
    ensureSchema(dbPath, HistoryGranularity::Gran1m);

    std::lock_guard<std::mutex> lk(m_writeMtx);
    m_writeConns[dbPath] = conn;        // 连接生命周期由 shared_ptr 管理，析构自动关闭
    return conn;
}
```

> **句柄泄漏防护**：所有连接以 `shared_ptr` 持有，析构时执行 `sqlite3_close`（或 `QSqlDatabase::close()` + `removeDatabase` 防止 Qt 连接句柄泄漏）。`ATTACH` 专用连接用完即 `DETACH` 后归还池（§3.3 RAII `scopeguard`）。

---

## 附录 A：表 — 文件 — 保留周期映射总览

| 逻辑表 | 物理文件 | 分片维度 | 写入者 | 保留 | 关键索引 |
|--------|----------|----------|--------|------|----------|
| `point_table` | `meta.db` | — | 配置热加载 | 永久 | (link_id, slave_address) |
| `history_1s_YYYYMM` | `data_YYYYMM.db` | 月 | `PersistThread` | 180 天 | PRIMARY KEY (point_id, ts) |
| `history_5s_YYYYMM` | `data_YYYYMM.db` | 月 | `PersistThread` | 180 天 | PRIMARY KEY (point_id, ts) |
| `history_1m_YYYYMM` | `data_YYYYMM.db` | 月 | `PersistThread` | 180 天 | PRIMARY KEY (point_id, ts) |
| `alarm_record_YYYYMM` | `alarm_YYYYMM.db`（独立） | 月 | `PersistThread` 消费队列（推荐）<br>`AlarmPersistThread`（高负载） | ≥ 365 天 | (point_id, trigger_time), (level, status) |
| `blackbox_snapshot` | `blackbox.db` | — | `BlackBoxManager` | 永久 | (alarm_id), (point_id) |
| `audit_log_YYYYMM` | `audit_YYYYMM.db` | 月 | `PersistThread` 消费队列（默认）<br>`AuditPersistThread`（隔离） | ≥ 3 年 | (timestamp), (user) |
| `users` / `global_kv` | `meta.db` | — | 用户管理 | 永久 | username(UNIQUE) |

## 附录 B：可追溯性矩阵（DBDD ↔ HLD / SRS）

| DBDD 章节 | 来源文档与条目 |
|-----------|----------------|
| §1.2 量化指标 | SRS FR-DLM-01~08、FR-AL-01~13、NFR-PERF-12；HLD §3.2 / §5 |
| §2.2 Sample/RingBuffer | HLD §5.3（ADR-08 alignas(16)、is_always_lock_free V1.4） |
| §2.3 critical_swap.dat | HLD §3.2.3（SWAP_FILE_SIZE/SLOT_SIZE/SwapHeader/SwapSlot）；非功能 §3.2.4 Torn-Write |
| §2.4 目录布局 | HLD §3.2.4.1 磁盘目录 |
| §3.2 路由 | HLD §3.2.4.1 `getTableName`/`getDatabasePath`/`batchInsertHistory`（V1.1） |
| §3.3 ATTACH 跨月 | HLD §3.2.4.2（V1.3）+ V1.4 ≤3 月上限 |
| §4.1 point_table | ICD V1.5 `PointTableEntry`/`PointTableConfig` |
| §4.2 降采样 | HLD `DownSampledSample`（max/min/avg/count） |
| §4.3 黑匣子 | HLD `insertBlackBox` + 蓝图黑匣子定义 |
| §4.4 告警 | HLD `AlarmRecord`/`AlarmLevel`/`AlarmStatus` + SRS FR-AL-08/09/13 |
| §4.5 审计 | SRS FR-AUTH-04（写操作留痕、不可篡改） |
| §5.1 WAL PRAGMA | HLD §5 + 非功能 §2.2.2 |
| §5.3 磁盘熔断 | HLD V1.4（<1GB / <200MB） |
| §5.4 WAL 饥饿防御 | 非功能 §2.6.4（Hard Timeout + RESTART/TRUNCATE V1.2） |
| §6 DAL 接口 | HLD `IDataAccess`/`SQLiteDataAccess`（V1.1~V1.4） |

## 附录 C：修订历史

| 版本 | 日期 | 作者 | 说明 |
|------|------|------|------|
| V1.0 | 2026-08-11 | 数据与业务开发工程师 | 首次发布，依据 HLD V1.5 / SRS / 非功能保障 / ICD 整合 L1~L2 全量数据库设计 |
| V1.1 | 2026-08-11 | 数据与业务开发工程师 | 采纳评审意见 3 条：① §4.3 黑匣子快照由 JSON TEXT 改为二进制 BLOB 存储原始 `Sample` 数组（16B/帧，零拷贝回放）；② §5.2 `DoubleBuffer` 增加容量上限（`kMaxBacklog`）与背压/丢弃策略；③ §3.3/§5.2 所有写入事务统一使用 `BEGIN IMMEDIATE`，规避并发同月库的 Shared→Exclusive 锁升级死锁与 `SQLITE_BUSY` |
| V1.2 | 2026-08-11 | 数据与业务开发工程师 | 采纳评审意见 2 条：① §3.3 `ReadOnlyConnectionPool::release()` 在归还前查询 `PRAGMA database_list` 并 `DETACH` 所有非 `main` 库，外层 `scopeguard` 保证异常路径也触发清理，防止 `database name is already in use`；② §5.2 `DoubleBuffer` 在触发背压丢弃时通过事件总线发出 Warn 级系统日志（含累计丢帧/队列长度/策略），并在状态栏暴露 `history.drop_count` / `history.queue_size` 计数器，避免运维误以为是前端采集中断 |
| V1.3 | 2026-08-11 | 数据与业务开发工程师 | 采纳评审意见 1 条：§5.2 持久化线程批量落库周期由 100ms 默认调整为 **1000ms**（可配置 500ms~1000ms），降低 `BEGIN IMMEDIATE` 获取 Reserved 锁的频率，缓解 WAL 锁竞争对只读查询的短暂阻塞；同步在 `config/runtime.json` 增加 `persist.flush_interval_ms` / `double_buffer_capacity` / `begin_immediate` 配置项，并在文档中给出 100ms/500ms/1000ms 周期 trade-off 表 |
| V1.4 | 2026-08-11 | 数据与业务开发工程师 | 采纳评审意见 3 条：① §4.2 降采样历史表改为 `WITHOUT ROWID` + `PRIMARY KEY (point_id, ts)`，消除 ROWID 主键树 + UNIQUE 隐式索引 + 显式索引的三重维护开销，节省 30%~50% 存储并提升 20%+ 批量写入性能；② §3.2/§5.2 批量写入由 `INSERT OR REPLACE` 改为标准 `INSERT INTO`（重复概率极低），需要覆写时使用 `ON CONFLICT(point_id, ts) DO UPDATE SET ...`，避免 REPLACE 底层 DELETE+INSERT 造成的索引节点删除/重建与 WAL 膨胀；③ 新增 §3.2.1 写入互斥控制，明确 `PersistThread` 为 `data_YYYYMM.db` 历史表唯一写入者，告警/审计通过消息队列串行化，高频（≥50/s）时启用 `alarm_YYYYMM.db` 独立库，避免文件级写锁互斥 |
| V1.5 | 2026-08-11 | 数据与业务开发工程师 | 采纳潜在隐患/待商榷点评审意见 4 条：① §3.2/§5.2 批量写入前按 `(point_id, ts)` 升序排序，避免 `WITHOUT ROWID` 表在乱序/补算数据入库时的 B-Tree 页分裂；② §2.3 Critical 告警触发瞬间调用 `syncBlocking()`（`msync(MS_SYNC)` / `FlushFileBuffers`），明确硬断电保护边界（单次 < 2ms）；③ §3.2.1/§4.4 告警库由动态隔离改为静态隔离，`alarm_record_YYYYMM` 默认且固定存放于独立 `alarm_YYYYMM.db`，简化路由与运维；④ §5.4 WAL 强制 checkpoint 由专用 `AdminConn` 线程在低峰期且无活跃只读连接时触发，运行期优先 `PASSIVE`，避免 `TRUNCATE` 与 UI 长查询互斥导致 `BEGIN IMMEDIATE` 卡顿 |
| V1.6 | 2026-08-11 | 数据与业务开发工程师 | 采纳潜在隐患评审意见 2 条：① §2.4/§5.3 冷存储归档清理前增加句柄驱逐（`ReadOnlyConnectionPool::evict()` / `SQLiteDataAccess::closeConnection()`）、显式 `close()`、`FileHandleTracker` 引用计数归零检查，避免 Windows `Access Denied` 与 Linux inode 不释放；② §3.3 只读连接池增加 FIFO 请求排队（`request_queue_max`）、UI 300ms 防抖 + 同范围请求合并（`ui_debounce_ms` / `ui_merge_requests`），避免 Size=4 在多客户端多通道趋势对比下耗尽并触发串行降级 |
| V1.7 | 2026-08-12 | 数据与业务开发工程师 | 采纳潜在隐患评审意见 1 条：§5.2.1 新增乱序/补算数据（Out-of-Order）处理策略，包含 5 层防御：业务层补算批次限频、`isBackfill` + `hasOutOfOrderInBatch()` DAL 标记、临时提升 `cache_size` 到 256MB、`PRAGMA optimize` 更新统计信息、低峰期 `REINDEX`/`VACUUM` 修复页分裂；同步在 `config/runtime.json` 增加 `backfill_rate_limit_per_minute` / `backfill_cache_size_override` / `backfill_reindex_window_hours` 配置项 |

> **待评审点**（供评审会确认）：
> 1. §4.2 列名 `v_max/v_min/v_avg/sample_count` 与 HLD 参考查询 `value/qmin/qmax/qavg` 的归一化（已在本文档统一，需确认上层查询代码同步更名）；
> 2. 告警记录保留期（≥ 365 天）与审计（≥ 3 年）的具体归档/冷存策略；
> 3. `alarm_record` 已采用静态隔离策略：固定存于独立 `alarm_YYYYMM.db`（V1.5），需确认目录布局与部署脚本同步调整；
> 4. 点表 `config_version` 的版本冲突解决策略（全量 UPSERT 覆盖 vs 增量合并）。
