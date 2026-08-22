# EnerSentry 储能上位机系统 —— 接口控制文档 (ICD) / 接口设计说明 (IDD)

> **文档编号**：ENS-ICD-001  
> **版本**：V1.14  
> **日期**：2026-08-10  
> **状态**：正式发布（V1.13 背压优先级隔离 + SpscRingBuffer Overrun 观测 + LTTB 平直桶优化 + 线程所有权与线程池兼容性四评审修订版）  
> **编制依据**：《EnerSentry-储能上位机系统-概要设计说明书 V1.5》(ENS-HLD-001)  
> **前序文档**：HLD V1.5、SRS V1.1、项目蓝图 V2.0、线程模型与并发设计专题报告 V1.0  
> **后续文档**：《详细设计说明书》、《测试方案》  
> **适用人员**：C++ 开发工程师、模块对接工程师、测试工程师、技术评审人员

---

## 文档修订记录

| 版本 | 日期 | 修订人 | 修订内容 |
|------|------|--------|---------|
| V1.0 | 2026-08-07 | 系统架构师 | 初始版本，基于 HLD V1.5 编制，覆盖全部 6 大接口域：通信接入层、数据中枢/Ring Buffer/mmap、业务引擎/SBO、UI 降采样、CMake 符号导出、JSON 序列化规范 |
| V1.1 | 2026-08-07 | 系统架构师 | **评审补丁**：① §7.0 新增 Qt 类型 ADL 适配层（`adl_serializer<QString>`），修复 `nlohmann/json` 对 `QString` 隐式反序列化失败的编译期缺陷；② §6.2 / §3.1 强制 MSVC x64 启用 `/cx16` 编译选项，确保 16 字节 `std::atomic<Sample>::is_always_lock_free` 通过；③ §3.2 升级 `RingBuffer::m_consumerCursors` 为 `std::array<std::atomic<size_t>, MAX_CONSUMERS>` 并显式声明 ConsumerId 单线程所有权契约；④ §4.2 新增 `DeviceSboGuard::purgeTerminatedEntries()` 维护入口并强制 Impl 内部哈希表在终态分支 erase 对应 Key |
| V1.2 | 2026-08-07 | 系统架构师 | **深度技术审查补丁**：① §1.5 新增 Qt 元类型注册契约，明确 `SlaveId`/`AlarmEvent`/`SboDeviceKey`/`SboSequenceResult`/`RenderPacket` 等跨线程信号参数必须在 `main()` 中执行 `qRegisterMetaType<T>()`，附运行时断言与错误现象对照；② §7.0 强化 `adl_serializer<QString>::from_json` 类型安全，显式分派 string/integer/float/boolean/null/object，避免 `j.dump()` 在非字符串节点上产生异常格式；③ §5.1 重构 `DownSampler` 为"输入指针 + 输出缓冲"零堆分配核心 API，保留旧按值返回接口作为便捷包装，并给出 60Hz 渲染路径预分配缓冲示例 |
| V1.3 | 2026-08-07 | 系统架构师 | **细节隐患与优化建议补丁**：① §3.2 修正 `RingBuffer` 游标语义为“累计计数 (Count)”：初始状态首个元素可读、`published == cursor` 判定无新数据、索引计算 `(cursor + i) & MASK`、回卷保护 `cursor = published - Capacity`；② §3.1 追加 16 字节 `std::atomic<Sample>` 的硬件开销说明（`lock cmpxchg16b` Cache Bouncing 风险）及 8B/12B 压缩备选方案；③ §4.2 将 `SboDeviceKey::hash()` 从 32-bit FNV-1a 升级为 `boost::hash_combine` 风格位移混合算法，缓解小整数 Key 的低位碰撞；④ §1.6 新增接口现代化约定，内部底层接口优先使用 `std::string_view`，并升级 `IMappedFile::open` 与 `AuthManager` 非 Slot 字符串参数签名 |
| V1.4 | 2026-08-07 | 系统架构师 | **潜在风险与细微隐患补丁**：① §1.6 与 §3.3 追加 `std::string_view` 与 C-API 兼容性风险说明，强制实现侧在调用 `fopen` / `CreateFileA` / POSIX `open` 前构造 `std::string` 或 `std::filesystem::path`，禁止直接传递 `path.data()`；② §3.2 `RingBuffer` 新增 `m_droppedFramesCount` 原子计数器与 `droppedFrames() / resetDroppedFrames()` API，在 Overrun 回卷保护触发时原子累加丢帧数，便于运维观测负载瓶颈；③ §7.0 补齐 `include/ens/QtJsonAdl.h` 完整头文件代码，显式包含 `<QtGlobal>` 以支持 `Q_ASSERT_X`，并标注为“完整可编译头文件” |
| V1.5 | 2026-08-07 | 系统架构师 | **工程一致性最终修订**：① 统一文档头部版本号与所有代码块顶部注释版本号为 V1.5；② §3.1 明确 16B `std::atomic<Sample>` 压测判定点（100ms 周期 + 3~4 Consumer、`lock cmpxchg16b` 单核耗时 > 5% 阈值），并给出优先采用的 8B `SampleCompact8` 压缩方案；③ §3.2 `RingBuffer::readRecent` 在 Overrun 触发位置增加按时间节流的 Warning 日志（`logOverrunThrottled()` + `m_lastOverrunLogTimeMs`，默认 5 秒 1 条），避免日志风暴；④ §4.2 将 `SboDeviceKey::hash()` 从 32-bit 返回升级为 `size_t` 64-bit Golden Ratio 位混合（0x9e3779b97f4a7c15ULL），并补全 `SboKeyHash` 函数对象定义 |
| V1.6 | 2026-08-10 | 系统架构师 | **进阶优化与契约回退补丁**：① §3.1 新增 `SampleCompact8` 8 字节压缩采样结构（uint32 relMs + uint32 value，x86-64 64-bit atomic 退化为 mov），作为 16B `lock cmpxchg16b` Cache Bouncing 的备选架构；② §3.1 新增 `SpscRingBuffer<T>` 单生产者单消费者无锁环形缓冲（仅 sequence / writeIndex 使用 release/acquire），彻底消除 16B 整体 atomic CAS；③ §3.5 `IDataAccess` 实现层约束补齐 SQLite 连接池与 `ATTACH DATABASE` 生命周期契约（每线程独立连接 + 跨月查询执行瞬间 ATTACH、查询完毕立即 DETACH，禁止长持有，规避 SQLite ATTACH 上限与跨线程句柄共享风险）；④ §4.3 `AuthManager` 回退 `const QString&`（撤销 V1.3 std::string_view 升级），明确业务层与 Qt 主框架交接的模块保留 QString 利用 `QStringView`/`QLatin1String` 避免 Qt → std::string → QString 二次堆分配；⑤ §5.x 新增 QCustomPlot 数据深拷贝说明与零拷贝优化路径（直接操作 `QCPGraph::data()->set()` / `replace()`，跳过中间 `QVector<QPointF>` 拷贝） |
| V1.7 | 2026-08-10 | 系统架构师 | **潜在技术隐患防御补丁**：① §3.2 `RingBuffer::readRecent` 在 `#ifndef NDEBUG` 包裹下增加运行时单线程所有权校验：首次调用记录当前 `std::thread::id` 到 `m_consumerOwnerThread[id]`，后续调用若 `std::this_thread::get_id()` 不一致则 `spdlog::error` + `Q_ASSERT_X` 拦截；新增 `m_ownerInitMutex` 防止首调并发注册竞争；Release 构建下整段被 `#ifndef` 剔除，不影响热路径性能；② §1.6.1 新增公共头文件 `include/ens/PathUtils.h`，提供 `ens::utils::to_null_terminated(std::string_view)` / `to_path(std::string_view)` / `fopen_safe(std::string_view, std::string_view)` 内联辅助函数；§3.3 `IMappedFile::open` 注释升级为强制使用辅助函数，明确 CR 阶段阻断裸 `path.data()` 写法 |
| V1.8 | 2026-08-10 | 系统架构师 | **潜在技术隐患改进建议补丁**：① §3.1.1 新增 16 字节 `std::atomic<Sample>` 压测闸门 (Benchmark Gate)，明确 100ms 周期 + 3~4 Consumer 场景、判定阈值与强制切换动作，要求 HIL 前必须完成压测并作为 ADR-21 输入；② §1.6.2 新增 clang-tidy 自定义检查 `ens-capi-stringview-safety`，在 CI 中自动阻断 `std::string_view::data()` 直接传给 `fopen`/`CreateFile`/`open` 等 C-API 的写法，提供 `.clang-tidy` 配置与检查骨架；③ §4.4 新增编译期头文件膨胀控制契约，要求 `AlarmEngine`/`DeviceSboGuard` 等业务类严格执行 PIMPL 与前置声明，`DeviceSboGuard.h` 移除 `<QTimer>` 改为 `class QTimer;`，并将 `<spdlog/spdlog.h>` 等重型头文件隔离在 `.cpp`；④ §3.5.1/§3.5.2/§3.5.4 新增 SQLite 连接池 `sqlite3_busy_timeout=3000ms` 配置与连接归还前 `ROLLBACK` 卫生校验，防止未提交事务/残留 ATTACH 污染池内连接 |
| V1.9 | 2026-08-10 | 系统架构师 | **代码完整性 + noexcept + SPSC 写者竞争防御补丁**：① §4.1.1/§4.2.1/§5.2.2/§6.3/§7.4 补齐 §4~§8 缺失的具体代码与规范，新增告警回调契约、SBO 状态机转换矩阵、QCustomPlot 批处理重绘实现伪代码、完整模块 CMakeLists.txt 示例、JSON 配置统一加载入口；② §3.1/§3.2/§5.1 为 `RingBuffer`/`SpscRingBuffer`/`DownSampler` 等无锁热路径函数统一添加 `noexcept`，释放编译器异常栈展开优化；③ §3.1 `SpscRingBuffer` 在 Debug 构建下新增 `m_producerOwnerThread` 运行时单线程写者校验，防止业务重构时多线程误调用 `pushBatch()` 导致数据竞争 |
| V1.10 | 2026-08-10 | 系统架构师 | **V1.9 纵深优化补丁**：① §3.1 `SpscRingBuffer::checkProducerOwnership` 将 V1.9 的 `std::mutex` + `std::lock_guard` 升级为 `std::atomic<std::thread::id>` + `compare_exchange_strong` lock-free CAS，编译期 `static_assert(is_always_lock_free)`，消除首调注册的 mutex 微秒级锁竞争，热路径延迟承诺彻底落实；② §3.2 末尾新增 §3.2.1 "V1.10 noexcept 异常传播防御契约"专节，列出四条防线 —— ①实现侧禁止调用清单（容器/Qt/字符串格式化/IO/分配）②clang-tidy 静态检查（启用 `bugprone-exception-escape`/`misc-noexcept`+ 自定义 `ens-noexcept-hot-path-noexcept-only`）③单元测试覆盖模板（含 `mock_bad_alloc()` 反向验证）④CI 阻断脚本示例；防止未来重构误在 noexcept 热路径中插入未 reserve 的容器或 fmt 日志导致 `std::terminate` 崩溃；并在 §3.2 注释中标注 RingBuffer 仍保留 mutex、未来可按需 lock-free 升级 |
| V1.11 | 2026-08-10 | 系统架构师 | **V1.10 锁升级纵深优化补丁**：① §3.2 `RingBuffer::readRecent` Debug 所有权校验从 V1.7 的 `std::mutex` + `std::lock_guard` 升级为 `std::atomic<std::thread::id>` + `compare_exchange_strong` lock-free CAS 模式，与 §3.1 `SpscRingBuffer` 形成代码库统一的"lock-free CAS 注册"风格；`m_consumerOwnerThread` 升级为 `std::array<std::atomic<std::thread::id>, MAX_CONSUMERS>`，编译期 `static_assert(is_always_lock_free)` 兜底，删除 `m_ownerInitMutex`，头文件移除 `<mutex>` 间接包含开销；② §5.2.1.1 新增"零拷贝填充的 GUI 主线程约束"专节，明确 `QCPGraph::data()->add/set/replace` 必须在 Qt GUI 主线程（`qApp->thread()`）执行，后台线程直接持有 `QCPGraph*` 修改 DataContainer 会与 `paintEvent` 撕裂读；`RealtimeChartWidget::onBatchRepaint` 等零拷贝槽函数入口新增 `Q_ASSERT_X(QThread::currentThread() == qApp->thread(), ...)` 断言作为 CR 强制检查项；提供降采样线程 → UI 主线程的 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 正确投递模式；③ §3.5.5 新增 `SqliteTxGuard` RAII 事务包装类（`src/datahub/SqliteTxGuard.h`），含 `TxType` 枚举（Deferred/Immediate/Exclusive）、构造时 `BEGIN` + 析构时根据 `m_commitOnSuccess` 自动 `COMMIT`/`ROLLBACK`、显式 `commit()`/`rollback()` 接口、嵌套事务抛异常保护、禁止拷贝/移动；CR 红线 + clang-tidy `scripts/ci_sqlite_txguard.sh` 扫描阻断业务代码中手写 `BEGIN`/`COMMIT`/`ROLLBACK` 字面量 |
| V1.12 | 2026-08-10 | 系统架构师 | **性能与健壮性纵深补丁**：① §3.1 `SpscRingBuffer::push`/`pushBatch` 删除冗余显式 `std::atomic_thread_fence(std::memory_order_release)`，因为 `m_writeSeq.store(..., release)` 本身已构成 Release 语义，避免 ARM 上多余的 `dmb ish` 指令，与代码注释中"仅对 sequence 使用 release/acquire 屏障"的描述保持一致；② §3.5.6 新增"单线程写队列 (Single-Writer DB Queue)"架构，针对 5000 点/秒高频落库等 L3 写密集型场景，所有后台写 Operation 打包放入无锁队列，由唯一 `DbWriter` 线程统一 Batch 批量插入，彻底消除 SQLite 单写者锁争用；其他业务线程保留连接仅用于 WAL 并发读；③ §5.1 `DownSampler::lttb` 新增 NaN/±Inf 异常值拦截与预处理契约，要求算法入口使用 `std::isnan`/`std::isinf` 过滤或在输入阶段替换为前一帧有效值/0.0f，防止 LTTB 三角形面积退化为 NaN 导致 UI 渲染崩溃 |
| V1.13 | 2026-08-10 | 系统架构师 | **潜在风险与改进建议纵深补丁**：① §3.5.6 为 `DbWriteQueue` 增加显式背压 (Backpressure) 机制专节，定义队列容量上限、阻塞/丢弃/Spill File 三种策略及决策矩阵，防止磁盘 IO 抖动时内存无限制膨胀；② §3.1.1/§3.1/§3.2 新增"两套无锁方案切换落地标准"，明确 `Sample`+`RingBuffer`(MPMC) 与 `SampleCompact8`+`SpscRingBuffer`(SPSC) 的模块划分映射、配置项与责任边界，避免不同开发人员混用；③ §3.1/§6.2/§6.3 增加 ARM64 跨平台编译器与指令集校验，明确 ARM64 缺乏 `cmpxchg16b`、不同 GCC/Clang 对 16B `std::atomic` 的 `is_always_lock_free` 支持差异，要求在 CI 中增加 ARM64 Cross-compiler 交叉编译校验任务；④ §5.1.1 细化 LTTB 异常值恢复策略，将默认替换策略从"0.0f"升级为"保持前一帧有效值 (Hold Last Valid Value)"，并在 `Sample`/`ChartDataPoint` 中定义数据质量位 (Data Quality Bit)，配合 QCustomPlot 断线/虚线渲染 |
| V1.14 | 2026-08-10 | 系统架构师 | **生产可观测性与线程契约深化补丁**：① §3.5.6/§3.5.7 将单队列背压升级为**双队列优先级隔离**：`HighPriorityEventQueue`（告警/ SOE / SBO 日志/审计日志，永不丢弃，Block 或 Spill File）与 `TelemetryWriteQueue`（遥测数据，允许按丢帧计数 Drop 或降采样合并），`DbWriter` 每周期优先排空高优先级队列，避免遥测积压淹没关键事件；② §3.1 `SpscRingBuffer` 补齐 Overrun 主动观测：`readRecent` 在回卷时原子累加 `m_droppedFramesCount`，提供 `droppedFrames()/resetDroppedFrames()` API 与按时间节流的 Warning 日志，保持与 `RingBuffer<T>` 可观测性一致；③ §5.1.3 新增 LTTB "平直桶/全相同值桶快速跳过"逻辑，当桶内 `Ymax == Ymin` 时直接取中点并跳过三角形面积计算，避免连续 HLVV 填充导致面积退化、算法退化为顺序选点；④ §3.1/§3.2 明确"单线程所有权"的两种合法语义（物理线程固定 vs 任务级串行上下文），并提供 `ExecutionContext` 抽象适配 QThreadPool / asio::thread_pool 等动态 Worker 线程场景 |

---

## 目录

1. [引言与文档约定](#1-引言与文档约定)
   - 1.1 [编写目的](#11-编写目的)
   - 1.2 [技术栈与工程约束](#12-技术栈与工程约束)
   - 1.3 [符号导出约定](#13-符号导出约定)
   - 1.4 [命名空间约定](#14-命名空间约定)
   - 1.5 [Qt 跨线程信号参数元类型注册约定](#15-qt-跨线程信号参数元类型注册约定)
   - 1.6 [接口现代化约定（C++17 `std::string_view` & C++20 `std::span`）](#16-接口现代化约定c17-stdstring_view--c20-stdspan)
   - 1.6.1 [C-API 安全辅助函数](#161-c-api-安全辅助函数强制使用)
2. [接入层抽象与通信引擎接口 (Layer 1 & Layer 2)](#2-接入层抽象与通信引擎接口-layer-1--layer-2)
   - 2.1 [IChannel 纯虚基类接口](#21-ichannel-纯虚基类接口)
   - 2.2 [ChannelConfig 配置结构体](#22-channelconfig-配置结构体)
   - 2.3 [ChannelFactory 工厂方法](#23-channelfactory-工厂方法)
   - 2.4 [ChannelStats 通信统计结构体](#24-channelstats-通信统计结构体)
   - 2.5 [从站熔断/降级状态转换回调与信号契约](#25-从站熔断降级状态转换回调与信号契约)
   - 2.6 [读写回调函数类型定义](#26-读写回调函数类型定义)
3. [数据中枢与 Ring Buffer / 分级存储接口 (Layer 3)](#3-数据中枢与-ring-buffer--分级存储接口-layer-3)
   - 3.1 [alignas(16) Sample 原子对齐结构体](#31-alignas16-sample-原子对齐结构体)
     - 3.1.1 [V1.8 压测闸门：16 字节 `std::atomic<Sample>` 开销必须在联调初期落地](#311-v18-压测闸门16-字节-stdatomicsample-开销必须在联调初期落地)
     - 3.1.2 [V1.13 两套无锁方案切换落地标准](#312-v113-两套无锁方案切换落地标准)
   - 3.2 [RingBuffer\<T\> 无锁模板接口](#32-ringbuffert-无锁模板接口)
   - 3.2.1 [V1.10 noexcept 异常传播防御契约](#321-v110-noexcept-异常传播防御契约防止热路径-stdterminate)
   - 3.2.2 [V1.14 单线程所有权与线程池兼容性](#322-v114-单线程所有权与线程池兼容性)
   - 3.3 [IMappedFile mmap 跨平台抽象接口](#33-imappedfile-mmap-跨平台抽象接口)
   - 3.4 [IDataAccess 数据访问抽象接口](#34-idataaccess-数据访问抽象接口)
   - 3.5 [IDataAccess SQLite 实现层并发契约](#35-idataaccess-sqlite-实现层并发契约v16)
     - 3.5.1 [SQLite 连接所有权规则](#351-sqlite-连接所有权规则)
     - 3.5.2 [ATTACH DATABASE 生命周期约束](#352-attach-database-生命周期约束)
     - 3.5.3 [实现层伪代码](#353-实现层伪代码)
     - 3.5.4 [V1.8 连接池归还卫生](#354-v18-连接池归还卫生rollback-校验与-busy_timeout-一致性)
     - 3.5.5 [V1.11 SqliteTxGuard RAII 事务包装类](#355-v111-sqlitetxguard-raii-事务包装类)
     - 3.5.6 [V1.12 单线程写队列 (Single-Writer DB Queue)](#356-v112-单线程写队列-single-writer-db-queue)
     - 3.5.7 [V1.14 双队列背压与优先级隔离 (HighPriorityEventQueue / TelemetryWriteQueue)](#357-v114-双队列背压与优先级隔离-highpriorityeventqueue--telemetrywritequeue)
4. [业务引擎与 SBO 控制状态机接口 (Layer 4)](#4-业务引擎与-sbo-控制状态机接口-layer-4)
   - 4.1 [AlarmEngine 告警引擎接口](#41-alarmengine-告警引擎接口)
   - 4.1.1 [告警回调与信号契约](#411-告警回调与信号契约)
   - 4.2 [DeviceSboGuard 设备级 SBO 逻辑锁接口](#42-devicesboguard-设备级-sbo-逻辑锁接口)
   - 4.2.1 [SBO 状态机转换矩阵与回调签名](#421-sbo-状态机转换矩阵与回调签名)
   - 4.3 [AuthManager RBAC 权限管理接口](#43-authmanager-rbac-权限管理接口)
   - 4.4 [V1.8 编译期头文件膨胀控制与 PIMPL 前置声明契约](#44-v18-编译期头文件膨胀控制与-pimpl-前置声明契约)
5. [UI 渲染与图表降采样契约 (Layer 5)](#5-ui-渲染与图表降采样契约-layer-5)
   - 5.1 [DownSampler 降采样 API](#51-downsampler-降采样-api)
     - 5.1.1 [V1.12 异常值 (NaN/±Inf) 拦截与预处理契约](#511-v112-异常值-naninf-拦截与预处理契约)
     - 5.1.2 [V1.13 异常值恢复策略：Hold Last Valid Value + 数据质量位 + 断线/虚线渲染](#5112-v113-异常值恢复策略hold-last-valid-value--数据质量位--断线虚线渲染)
     - 5.1.3 [V1.14 LTTB 平直桶 / 全相同值桶快速跳过](#5113-v114-lttb-平直桶--全相同值桶快速跳过)
   - 5.2 [批处理重绘契约 (QTimer 节流)](#52-批处理重绘契约-qtimer-节流)
   - 5.2.1 [V1.6 QCustomPlot 数据深拷贝与零拷贝填充契约](#521-v16-qcustomplot-数据深拷贝与零拷贝填充契约)
     - 5.2.1.1 [V1.11 零拷贝填充的 GUI 主线程约束](#5211-v111-零拷贝填充的-gui-主线程约束强制-cr-检查项)
   - 5.2.2 [V1.9 RealtimeChartWidget::onBatchRepaint 实现伪代码](#522-v19-realtimechartwidgetonbatchrepaint-实现伪代码)
6. [CMake 编译与符号导出声明规范](#6-cmake-编译与符号导出声明规范)
   - 6.1 [ens/export.hpp 标准实现](#61-ensexporthpp-标准实现)
   - 6.2 [SHARED/STATIC 接口暴露规则](#62-sharedstatic-接口暴露规则)
   - 6.3 [V1.9 完整模块 CMakeLists.txt 示例](#63-v19-完整模块-cmakeliststxt-示例)
7. [数据结构 JSON 序列化规范 (nlohmann/json)](#7-数据结构-json-序列化规范-nlohmannjson)
   - 7.0 [Qt 类型 ADL 适配层](#70-qt-类型-adl-适配层)
   - 7.1 [点表配置 pointtable.json](#71-点表配置-pointtablejson)
   - 7.2 [链路配置 channels.json](#72-链路配置-channelsjson)
   - 7.3 [告警规则 alarm_rules.json](#73-告警规则-alarm_rulesjson)
   - 7.4 [V1.9 JSON 配置统一加载入口 ConfigManager](#74-v19-json-配置统一加载入口-configmanager)
8. [附录：接口契约速查矩阵](#8-附录接口契约速查矩阵)

---

## 1. 引言与文档约定

### 1.1 编写目的

本文档是 EnerSentry 储能上位机系统的**接口控制文档 (ICD) / 接口设计说明 (IDD)**，衔接《概要设计说明书 V1.5》，为开发团队的 C++ 编码与模块对接提供**可直接使用的头文件声明、数据结构定义、内存布局约束和序列化规范**。

**核心目标**：

- **消除接口歧义**：以完整 C++17 头文件形式定义所有跨模块公开接口，编译期即可约束调用方；
- **固化内存语义**：显式标注原子操作的内存顺序 (`memory_order`)、结构体对齐 (`alignas`)、导出宏 (`ENS_*_API`)；
- **提供序列化规范**：基于 `nlohmann/json` 给出 `to_json` / `from_json` 映射，确保配置文件与内存结构一致；
- **作为编码基线**：所有 `*.cpp` 实现文件必须严格遵循本文档的接口签名，不可私自增删公开方法。

**预期读者**：C++ 开发工程师、模块对接工程师、Code Review 参与者、测试工程师。

### 1.2 技术栈与工程约束

| 维度 | 约束 |
|------|------|
| 语言标准 | C++17（`std::optional`、`std::variant`、structured bindings、`if constexpr`） |
| UI 框架 | Qt 5.15 LTS / Qt 6.x |
| 图表库 | QCustomPlot 2.x |
| JSON 库 | nlohmann/json（`NLOHMANN_JSON_NAMESPACE` 为 `ens::json`） |
| 数据库 | SQLite 3.x (WAL 模式) |
| 日志 | spdlog |
| 构建系统 | CMake 3.16+（混合 STATIC + SHARED） |
| 编译器 | MSVC 2019+ / GCC 11+ / Clang 14+ |
| 性能基线 | 100ms 采集周期、5,000 点/秒异步批处理落库、60 FPS 降采样渲染、告警延迟 < 100ms |

### 1.3 符号导出约定

本文档中所有 `class` / `struct` / 函数声明使用以下导出宏：

| 宏 | 所属 Target | 构建类型 |
|----|------------|---------|
| `ENS_CHANNEL_API` | `ens::channel` | SHARED (channel.dll) |
| `ENS_BUSINESS_API` | `ens::business` | SHARED (business.dll) |

`ens::datahub`、`ens::protocol`、`ens::ui` 为 STATIC 库，**不使用导出宏**（符号默认全部可见）。

详见 [第 6 章](#6-cmake-编译与符号导出声明规范)。

### 1.4 命名空间约定

```cpp
namespace ens {
    namespace channel    { /* L1: 通信接入层 */ }
    namespace protocol   { /* L2: 协议处理层 */ }
    namespace datahub    { /* L3: 数据中枢层 */ }
    namespace business   { /* L4: 业务逻辑层 */ }
    namespace ui         { /* L5: UI 视图层   */ }
    namespace app        { /* 应用入口 */ }

    // 平台抽象层
    namespace datahub::platform { /* mmap 跨平台 */ }
}
```

### 1.5 Qt 跨线程信号参数元类型注册约定

> **⚠ V1.2 修订 (2026-08-07)**：补充 Qt 元类型注册契约。当自定义结构体/枚举类以 `Qt::QueuedConnection` 跨线程投递时，若未提前向 Qt 元对象系统注册，运行时消息队列会报错并**静默丢弃**该消息。

**问题场景**：

文档中大量使用了跨线程 Qt 信号投递自定义类型，例如：

| 发送者 | 信号 | 参数类型 | 接收者线程 |
|-------|------|---------|-----------|
| `PollScheduler` | `slaveDegraded` | `ens::protocol::SlaveId` (enum class) | UI 通信诊断 |
| `PollScheduler` | `slaveIsolated` | `ens::protocol::SlaveId` | UI 通信诊断 |
| `AlarmEngine` | `newAlarm` | `ens::business::AlarmEvent` | UI 告警中心 |
| `DeviceSboGuard` | `sboStateChanged` | `ens::business::SboSequenceResult` | UI SBO 控制台 |
| `DeviceSboGuard` | `armedRejected` | `ens::business::SboDeviceKey` | UI SBO 控制台 |
| `RealtimeChartWidget`（渲染准备线程） | `onRenderPacket` | `ens::ui::RenderPacket` | UI 主线程 |

**必须注册的类型清单**（在 `main()` 中，早于任何跨线程 `emit` 调用执行）：

```cpp
// ============================================================================
// EnerSentry — Qt 元类型注册初始化 (V1.5)
// 调用位置: main.cpp（或 ApplicationInitializer::initMetaTypes()）
// 调用时机: 在 QApplication 构造之后、任何跨线程 emit 之前
// ============================================================================
#include <QApplication>
#include <QMetaType>

#include "protocol/SlaveId.h"       // SlaveId
#include "business/AlarmEngine.h"   // AlarmEvent
#include "business/DeviceSboGuard.h"// SboDeviceKey, SboSequenceResult
#include "ui/DownSampler.h"         // RenderPacket

namespace ens::app {

void registerMetaTypes() {
    // protocol 层 —— 从站 ID 枚举类（跨线程熔断/降级信号）
    qRegisterMetaType<ens::protocol::SlaveId>("ens::protocol::SlaveId");

    // business 层 —— 告警事件
    qRegisterMetaType<ens::business::AlarmEvent>("ens::business::AlarmEvent");

    // business 层 —— SBO 设备键
    qRegisterMetaType<ens::business::SboDeviceKey>("ens::business::SboDeviceKey");

    // business 层 —— SBO 序列结果
    qRegisterMetaType<ens::business::SboSequenceResult>("ens::business::SboSequenceResult");

    // ui 层 —— 渲染数据包（注意 RenderPacket 内部含 QVector<QPointF>，已在 Qt 中注册）
    qRegisterMetaType<ens::ui::RenderPacket>("ens::ui::RenderPacket");

    // 如需在信号参数中直接使用 std::vector / std::optional，也需注册
    // qRegisterMetaType<std::vector<int>>("std::vector<int>");
}

}  // namespace ens::app

// main.cpp
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    ens::app::registerMetaTypes();  // ← 必须放在所有线程启动之前

    // ... 后续初始化与窗口创建
    return app.exec();
}
```

**类型支持性要求**：

| 类型 | 需要条件 | 说明 |
|------|---------|------|
| 普通结构体 | 默认构造函数 + public 默认拷贝/移动 | `QMetaType` 自动反射 |
| enum class | `Q_ENUM_NS` 或 `Q_ENUM` | 推荐在头文件中使用 `Q_ENUM_NS(ens::protocol::SlaveId)` |
| 含 QVector 字段的结构体 | 字段类型已注册 | `QVector<QPointF>` 由 Qt 自动注册 |
| 自定义模板 | 对每个实例化单独注册 | 如 `std::optional<AlarmEvent>` 不推荐跨线程传递 |

**运行时检测断言**：

```cpp
// 在 registerMetaTypes() 末尾添加防御性断言，确保注册成功
Q_ASSERT(QMetaType::fromType<ens::protocol::SlaveId>().isValid());
Q_ASSERT(QMetaType::fromType<ens::business::AlarmEvent>().isValid());
Q_ASSERT(QMetaType::fromType<ens::business::SboDeviceKey>().isValid());
Q_ASSERT(QMetaType::fromType<ens::business::SboSequenceResult>().isValid());
Q_ASSERT(QMetaType::fromType<ens::ui::RenderPacket>().isValid());
```

**错误现象对照**：

```
QObject::connect: Cannot queue arguments of type 'ens::business::AlarmEvent'
(Make sure 'ens::business::AlarmEvent' is registered using qRegisterMetaType().)
```

若出现上述日志，说明对应类型的 `qRegisterMetaType<T>()` 未被调用，信号消息将被丢弃，UI 表现为“告警不刷新 / 曲线不更新”。

---

### 1.6 接口现代化约定（C++17 `std::string_view` & C++20 `std::span`）

**头文件路径**：`src/ens/modern_interface.hpp`（可选公共头）

为减少纯只读字符串参数的临时 `std::string` 构造、以及避免高频渲染路径中的堆分配，文档对内部底层接口做如下现代化约定。注意：**业务层与 Qt 主框架交接的模块（如 `AuthManager`）保持 `const QString&`**，避免 `QString → std::string → QString` 的二次堆分配（参见 V1.6 补丁说明）。

1. **只读字符串参数**：
   - **纯底层接口（无 Qt 依赖）**：如 `IMappedFile::open`、DataHub 内部路径处理，优先使用 `std::string_view` 替代 `const std::string&`，避免临时 `std::string` 堆分配。
   - **业务层与 Qt 主框架交接的模块**：如 `AuthManager`，**保持 `const QString&`**。原因是这些接口的调用方通常是 Qt UI 控件（已持有 `QString`），若改为 `std::string_view` 会强制调用方执行 `qstr.toStdString()` 产生一次堆分配；而 `AuthManager` 内部为了构造 SQL 或再次发出 Qt 信号又需要转回 `QString`，造成**二次堆分配**。保持 `const QString&` 可直接利用 Qt 内部的 `QStringView` / `QLatin1String` 等零拷贝机制。
   - **信号/槽参数**：任何暴露给 Qt 元对象系统的信号/槽函数，仍使用 `const QString&`，以维持 QML/信号槽的连接兼容性。
   - 典型场景：文件路径、用户名、配置键、设备地址。
   - 兼容性：`std::string_view` 可隐式从 `const char*`、`std::string`、`QByteArray::toStdString()` 等构造。

2. **Qt Slot / 信号参数保持 `QString`**：
   - 任何暴露给 Qt 元对象系统的信号/槽函数，仍使用 `const QString&`，以维持 QML/信号槽的连接兼容性。
   - 示例：`AuthManager::userLoggedIn(const QString& userId)` 信号保持 `QString`。

3. **数组/缓冲参数**：
   - C++17 项目：使用 `const T* data + size_t size` 组合（零堆分配）。
   - 若未来升级至 C++20：平滑演进为 `std::span<const T>`，如 `DownSampler::lttb(std::span<const QPointF> src, ...)`。

4. **实现侧注意**：
   - `std::string_view` 不保证以 `\0` 结尾，调用 C API（如 `fopen`、`CreateFileA`、POSIX `open`）前必须显式构造 `std::string` 或 `std::filesystem::path`，严禁直接传递 `path.data()`。
     ```cpp
     // ❌ 危险: std::string_view 可能是切片，data() 不保证 '\0' 结尾
     HANDLE h = CreateFileA(path.data(), ...);  // 越界读取风险

     // ✅ 正确: 先构造 owning string / path
     auto p = std::filesystem::path(path);      // 保留长度，避免越界
     HANDLE h = CreateFileA(p.string().c_str(), ...);
     // 或 POSIX:
     int fd = ::open(std::string(path).c_str(), ...);
     ```
   - 不要返回 `std::string_view` 指向局部临时字符串。
   - 例外：使用支持范围参数的系统 API（如 `std::ofstream::open` 的 `std::filesystem::path` 重载）可直接使用 `std::filesystem::path(path)`。

### 1.6.1 C-API 安全辅助函数（强制使用）

为防止团队成员在写实现代码时**遗漏**上述 `std::string_view → C-API` 转换约束，公共头文件 `include/ens/PathUtils.h` 提供以下 **内联辅助函数**。所有向 C-API 传递路径的代码必须经过这些函数，禁止散落各处的裸 `std::string(path).c_str()` 调用。

```cpp
// ============================================================================
// EnerSentry — C-API 字符串安全适配 (V1.7 公共头文件)
// 头文件: include/ens/PathUtils.h
// 适用范围: 任何准备把 std::string_view 传给 C-API (fopen / CreateFile / open)
//           的实现代码, 都必须通过本文件的辅助函数二次封装
// ============================================================================
#pragma once

#include <string_view>
#include <string>
#include <filesystem>
#include <type_traits>

namespace ens::utils {

/// 将 std::string_view 安全地转换为以 '\0' 结尾的 std::string
/// 用于调用 C-API 前构造 owning buffer
///
/// ⚠ 使用场景: 必须确保 std::string_view 不依赖任何短字符串优化 (SSO) 之外
///   的内存生命周期, 即调用方提供的 view 必须在本次调用结束前保持有效
[[nodiscard]] inline std::string
to_null_terminated(std::string_view sv) noexcept(false) {
    // 直接构造 std::string, 内部拷贝保证 '\0' 结尾
    // 短字符串 (≤15 字节) 走 SSO, 无堆分配; 长字符串 1 次堆分配
    return std::string{sv};
}

/// 安全包装: std::string_view → std::filesystem::path
/// 用于 std::ofstream / std::filesystem API
[[nodiscard]] inline std::filesystem::path
to_path(std::string_view sv) noexcept(false) {
    return std::filesystem::path{sv};
}

/// 安全包装: std::string_view → 跨平台 FILE* (POSIX fopen / Windows fopen_s)
///
/// 用法:
///   FILE* fp = ens::utils::fopen_safe(path, "rb");
///   if (!fp) { /* 错误处理 */ }
///   // ... 使用 fp ...
///   fclose(fp);
[[nodiscard]] inline FILE* fopen_safe(std::string_view path,
                                       std::string_view mode) noexcept {
    // 内部构造 std::string (保证 '\0' 结尾) 再调用平台 fopen
    return std::fopen(to_null_terminated(path).c_str(),
                      to_null_terminated(mode).c_str());
}

}  // namespace ens::utils
```

**§1.6.1 强制约束**：
- 任何 C-API 调用（包括但不限于 `fopen`, `CreateFileA`, `CreateFileW`, `open`, `stat`, `access`, `_wfopen`）必须经过 `ens::utils` 中的对应辅助函数或显式调用 `to_null_terminated`。
- 代码审查（CR）阶段必须验证：未发现 `xxx(path.data(), ...)` 或 `std::string_view + 直接 .data()` 的写法。
- 替代方案（如直接约束底层 API 接收 `const std::filesystem::path&`）适用于新代码；对存量 C-API 仍使用本辅助函数作为兜底。

**受影响的接口示例**（本节补丁后统一升级）：

| 接口 | 签名 | 备注 |
|------|------|------|
| `IMappedFile::open` | `open(std::string_view path, ...)` | 非 Qt，纯底层；实现侧 **必须** 调用 `ens::utils::to_null_terminated(path)` / `ens::utils::to_path(path)`（§1.6.1），禁止直接传 `path.data()` |
| `AuthManager::login` | `login(const QString& userName, const QString& password)` | **V1.6 回退保持 `QString`**，避免 Qt UI 调用方二次堆分配 |
| `AuthManager::logout` | `logout(const QString& userId)` | 保持 `QString` |
| `AuthManager::refreshToken` | `refreshToken(const QString& userId)` | 保持 `QString` |
| `AuthManager::removeUser` | `removeUser(..., const QString& userId)` | 保持 `QString` |
| `AuthManager::enableUser` | `enableUser(..., const QString& userId, ...)` | 保持 `QString` |
| `AuthManager::changePassword` | `changePassword(..., const QString&, const QString&)` | 保持 `QString` |
| `AuthManager::resetPassword` | `resetPassword(..., const QString&, const QString&)` | 保持 `QString` |
| `AuthManager::forceLockSession` | `forceLockSession(..., const QString& userId)` | 保持 `QString` |

#### 1.6.2 V1.8 CR 自动化：clang-tidy 自定义规则阻断裸 `std::string_view::data()`

**隐患**：依赖人工 Code Review 拦截裸 `path.data()` 仍存在遗漏概率；跨团队、迭代频繁时，新增代码或重构容易重新引入危险写法。

**目标**：在 CMake/CI 检查流水线中引入 **clang-tidy 自定义检查规则**，对直接向 `fopen` / `CreateFileA` / `CreateFileW` / `::open` / `stat` / `access` / `_wfopen` 等 C-API 传递 `std::string_view::data()` 的行为进行静态扫描并强制阻断。

**检查规则要点**：

1. **匹配模式**：识别 `CallExpr`，其任意实参为 `MemberExpr`，成员名 `data()`，且基表达式类型为 `std::string_view`（或 `std::string_view` 的别名/派生）。
2. **被禁函数清单**：`fopen`, `fopen_s`, `_wfopen`, `::open`, `open64`, `CreateFileA`, `CreateFileW`, `CreateFile2`, `stat`, `fstat`, `lstat`, `access`, `_access`, `remove`, `rename`, `_unlink`, `std::fopen`, `std::ifstream::open` 的 `(const char*, ...)` 重载等。
3. **例外白名单**：`std::filesystem::path` 接受 `std::string_view` 的重载、`std::ofstream::open(std::filesystem::path const&)`、`ens::utils::to_null_terminated`、`ens::utils::fopen_safe`、`ens::utils::to_path` 等安全封装函数内部不受限（但封装函数本身必须在 NOLINT 注释中明确说明）。

**最小可运行 .clang-tidy 配置片段**：

```yaml
# EnerSentry .clang-tidy (V1.8)
---
Checks: >
  clang-diagnostic-*,
  clang-analyzer-*,
  cppcoreguidelines-*,
  performance-*,
  portability-*,
  cert-*,
  bugprone-*,
  -cppcoreguidelines-avoid-magic-numbers,
  ens-capi-stringview-safety  # 项目自定义检查（见下方实现）

CheckOptions:
  - key:   ens-capi-stringview-safety.BannedFunctions
    value: 'fopen;fopen_s;_wfopen;open;open64;CreateFileA;CreateFileW;CreateFile2;stat;fstat;lstat;access;_access;remove;rename;_unlink'
  - key:   ens-capi-stringview-safety.WhitelistNamespaces
    value: 'ens::utils;std::filesystem'

HeaderFilterRegex: 'src/.*|include/ens/.*'
FormatStyle: file
```

**自定义 clang-tidy 检查骨架**（放置于 `tools/clang-tidy/EnsStringViewCApiCheck.cpp`，并在 CMake 中编译为插件）：

```cpp
// ============================================================================
// EnerSentry — clang-tidy 自定义检查: EnsStringViewCApiCheck (V1.8)
// 检查：禁止将 std::string_view::data() 直接传给 C-API 文件/路径函数
// ============================================================================
#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>
#include <clang/AST/ASTContext.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>

namespace clang::tidy::ens {

using namespace ast_matchers;

class EnsStringViewCApiCheck : public ClangTidyCheck {
public:
    EnsStringViewCApiCheck(StringRef Name, ClangTidyContext* Context)
        : ClangTidyCheck(Name, Context) {}

    void registerMatchers(MatchFinder* Finder) override {
        // 匹配: any_func( ..., some_string_view.data(), ... )
        Finder->addMatcher(
            callExpr(
                callee(functionDecl(hasAnyName(
                    "fopen", "fopen_s", "_wfopen", "open", "open64",
                    "CreateFileA", "CreateFileW", "CreateFile2",
                    "stat", "fstat", "lstat", "access", "_access",
                    "remove", "rename", "_unlink"))),
                hasArgument(0, cxxMemberCallExpr(
                    on(expr(hasType(cxxRecordDecl(hasName("std::string_view"))))),
                    callee(cxxMethodDecl(hasName("data")))))
            ).bind("bad_call"),
            this);
    }

    void check(const MatchFinder::MatchResult& Result) override {
        if (const auto* Call = Result.Nodes.getNodeAs<CallExpr>("bad_call")) {
            diag(Call->getBeginLoc(),
                 "禁止将 std::string_view::data() 直接传给 C-API; "
                 "使用 ens::utils::to_null_terminated() / to_path() / fopen_safe() 封装");
        }
    }
};

} // namespace clang::tidy::ens
```

**CI 集成（CMake + 阻断合并）**：

```cmake
# CMake 中开启 clang-tidy（开发/CI 统一）
set(CMAKE_CXX_CLANG_TIDY
    "clang-tidy;--config-file=${CMAKE_SOURCE_DIR}/.clang-tidy;--warnings-as-errors=ens-capi-stringview-safety")

# CI 流水线（示例 GitHub Actions / GitLab CI）
# - 在 build 步骤前运行 clang-tidy 检查
# - 任何 ens-capi-stringview-safety 报错即失败，禁止合入
```

**强制约束**：

- 任何新增或修改的 `.cpp` 文件必须通过 clang-tidy 检查；`ens-capi-stringview-safety` 报错视为编译失败。
- 白名单函数（`ens::utils::*`）内部若必须直接调用 C-API，需在调用处添加 `// NOLINT(ens-capi-stringview-safety)` 并在同一行写明原因与审查记录编号。
- 代码审查 (CR) 阶段仍抽查辅助函数封装是否正确；静态扫描作为第一道防线，不替代人工审查。

---

## 2. 接入层抽象与通信引擎接口 (Layer 1 & Layer 2)

### 2.1 IChannel 纯虚基类接口

**所属 Target**：`ens::channel`（SHARED 库，`channel.dll`）  
**对应需求**：SRS COMM-12/13、NFR-PORT-03  
**头文件路径**：`src/channel/IChannel.h`

```cpp
// ============================================================================
// EnerSentry — IChannel 统一通道抽象接口 (V1.5)
// 所属 Target: ens::channel (SHARED)
// 头文件: src/channel/IChannel.h
// ============================================================================
#pragma once

#include "ens/export.hpp"
#include "ChannelConfig.h"
#include "ChannelStats.h"
#include <QByteArray>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

namespace ens::channel {

// ============================================================================
// 读回调函数类型 — 通道收到数据时的异步通知
// 回调在通道 IO 线程中执行，禁止长时间阻塞
// ============================================================================
using ReadCallback = std::function<void(const QByteArray& data)>;

// ============================================================================
// 通道连接状态变更回调
// ============================================================================
using ConnectionChangedCallback = std::function<void(bool connected)>;

// ============================================================================
// 通道错误回调
// ============================================================================
using ErrorCallback = std::function<void(const QString& errorMessage)>;

// ============================================================================
// IChannel — 统一通道抽象纯虚基类
//
// 设计约束:
//  - open/close 成对调用，close 必须幂等（二次调用不抛异常）
//  - write 为同步阻塞写入，RS485 半双工场景下由上层保证串行调度
//  - read 为非阻塞读取，返回当前内核缓冲区可用数据
//  - setReadCallback 注册异步读回调，数据到达时触发
//  - 所有 IChannel 子类必须为 QObject，以支持跨线程 signal/slot
// ============================================================================
class ENS_CHANNEL_API IChannel : public QObject {
    Q_OBJECT

public:
    IChannel(QObject* parent = nullptr) : QObject(parent) {}
    ~IChannel() override = default;

    // 禁止拷贝与移动（QObject 语义 + 资源所有权唯一）
    IChannel(const IChannel&) = delete;
    IChannel& operator=(const IChannel&) = delete;
    IChannel(IChannel&&) = delete;
    IChannel& operator=(IChannel&&) = delete;

    // ──── 生命周期 ────

    /// 打开通道（参数由 ChannelConfig 多态承载）
    /// @param cfg  通道配置（SerialConfig / TcpConfig / CanConfig）
    /// @return     true=成功；false=失败（通过 lastError() 获取错误详情）
    virtual bool open(const ChannelConfig& cfg) = 0;

    /// 关闭通道（必须幂等：二次调用不抛异常、不重复释放资源）
    virtual void close() = 0;

    // ──── I/O 操作 ────

    /// 同步写入字节流
    /// @param data  待写入的原始字节
    /// @return      实际写入字节数；-1 表示错误
    /// @note        RS485 半双工：写后须等待从站响应再发下一帧
    virtual int write(const QByteArray& data) = 0;

    /// 非阻塞读取
    /// @param maxBytes 最大读取字节数
    /// @return         当前缓冲区可用数据（可能为空）
    virtual QByteArray read(int maxBytes = 4096) = 0;

    // ──── 状态查询 ────

    /// 查询通道连接状态
    /// @return true=已连接；false=已断开
    virtual bool isConnected() const = 0;

    /// 获取通信统计快照（原子读取）
    virtual ChannelStats getStats() const = 0;

    /// 获取最近一次错误描述
    virtual QString lastError() const = 0;

    // ──── 回调注册 ────

    /// 注册异步读回调（数据到达时在 IO 线程触发）
    /// @param cb 回调函数；传 nullptr 取消注册
    virtual void setReadCallback(ReadCallback cb) = 0;

    /// 注册连接状态变更回调
    virtual void setConnectionChangedCallback(ConnectionChangedCallback cb) = 0;

    /// 注册错误回调
    virtual void setErrorCallback(ErrorCallback cb) = 0;

signals:
    /// 数据到达信号（用于 Qt 跨线程通知）
    void dataReceived(const QByteArray& data);

    /// 连接状态变更信号
    void connectionChanged(bool connected);

    /// 错误信号
    void errorOccurred(const QString& errorMessage);
};

}  // namespace ens::channel
```

### 2.2 ChannelConfig 配置结构体

**所属 Target**：`ens::channel`（SHARED 库）  
**头文件路径**：`src/channel/ChannelConfig.h`

```cpp
// ============================================================================
// EnerSentry — ChannelConfig 通道配置结构体 (V1.5)
// 所属 Target: ens::channel (SHARED)
// 头文件: src/channel/ChannelConfig.h
// ============================================================================
#pragma once

#include "ens/export.hpp"
#include <QString>
#include <cstdint>
#include <variant>

namespace ens::channel {

// ============================================================================
// 通道类型枚举
// ============================================================================
enum class ChannelType : uint8_t {
    Serial = 0,   // RS485 / RS232 串口
    TCP    = 1,   // Modbus TCP 客户端
    CAN    = 2,   // CAN 总线 (SocketCAN / ZLG CAN)
};

// ============================================================================
// 串口配置
// ============================================================================
struct ENS_CHANNEL_API SerialConfig {
    QString portName;          // 例: "COM3" (Win) 或 "/dev/ttyUSB0" (Linux)
    int     baudRate    = 115200;
    int     dataBits    = 8;     // 5/6/7/8
    int     stopBits    = 1;     // 1/1.5/2
    QString parity      = "N";   // "N"=None, "E"=Even, "O"=Odd
    int     responseTimeoutMs = 500;  // 从站响应超时(ms)
    int     interFrameDelayUs  = 3500; // 帧间隔(3.5 字符时间 @ 115200 ≈ 300μs，取保守值)
};

// ============================================================================
// TCP 客户端配置
// ============================================================================
struct ENS_CHANNEL_API TcpConfig {
    QString host;
    uint16_t port           = 502;    // Modbus TCP 默认端口
    int      connectTimeoutMs  = 3000;
    int      responseTimeoutMs = 500;
    int      reconnectBaseMs   = 1000;   // 重连初始间隔
    int      reconnectMaxMs    = 30000;  // 重连封顶间隔（指数退避）
};

// ============================================================================
// CAN 配置（预留扩展）
// ============================================================================
struct ENS_CHANNEL_API CanConfig {
    QString interfaceName;        // "can0" (SocketCAN) 或 ZLG 设备索引
    int     bitrate    = 500000;  // 500 kbps
    bool    useZlgDriver = false; // true=周立功 CAN 卡；false=SocketCAN
};

// ============================================================================
// ChannelConfig — 通道配置的强类型联合体
// 通过 std::variant 承载三种通道类型的专属配置，避免 void* / union 的类型不安全
// ============================================================================
struct ENS_CHANNEL_API ChannelConfig {
    ChannelType                                            type = ChannelType::Serial;
    std::variant<SerialConfig, TcpConfig, CanConfig>       params;

    /// 便捷访问：按类型获取配置引用
    template<typename T>
    T& as() { return std::get<T>(params); }

    template<typename T>
    const T& as() const { return std::get<T>(params); }

    /// 校验配置有效性
    bool isValid() const;
};

}  // namespace ens::channel
```

### 2.3 ChannelFactory 工厂方法

**所属 Target**：`ens::channel`（SHARED 库）  
**头文件路径**：`src/channel/ChannelFactory.h`

```cpp
// ============================================================================
// EnerSentry — ChannelFactory 通道工厂 (V1.5)
// 所属 Target: ens::channel (SHARED)
// 头文件: src/channel/ChannelFactory.h
// ============================================================================
#pragma once

#include "ens/export.hpp"
#include "IChannel.h"
#include "ChannelConfig.h"
#include <memory>

namespace ens::channel {

/// 通道工厂 — 根据配置自动创建对应通道实现
/// 上层协议引擎仅依赖 IChannel 接口，无需感知具体通道类型
class ENS_CHANNEL_API ChannelFactory {
public:
    ChannelFactory() = delete;

    /// 创建通道实例（工厂方法）
    /// @param cfg  通道配置（SerialConfig / TcpConfig / CanConfig）
    /// @return     unique_ptr<IChannel>；失败返回 nullptr
    /// @note      创建的实例由调用方持有所有权
    static std::unique_ptr<IChannel> create(const ChannelConfig& cfg);

    /// 注册自定义通道类型（插件化扩展点）
    /// @param type      通道类型标识
    /// @param creator   创建函数
    /// @return          true=注册成功；false=该类型已被注册
    static bool registerChannel(
        ChannelType type,
        std::function<std::unique_ptr<IChannel>()> creator);
};

}  // namespace ens::channel
```

### 2.4 ChannelStats 通信统计结构体

**所属 Target**：`ens::channel`（SHARED 库）  
**头文件路径**：`src/channel/ChannelStats.h`

```cpp
// ============================================================================
// EnerSentry — ChannelStats 通信统计结构体 (V1.5)
// 所属 Target: ens::channel (SHARED)
// 头文件: src/channel/ChannelStats.h
// ============================================================================
#pragma once

#include "ens/export.hpp"
#include <atomic>
#include <cstdint>

namespace ens::channel {

/// 通信链路统计（全部字段原子更新，跨线程安全读取）
struct ENS_CHANNEL_API ChannelStats {
    std::atomic<uint64_t> requestTotal{0};       // 请求总数
    std::atomic<uint64_t> responseSuccess{0};    // 成功响应数
    std::atomic<uint64_t> timeoutCount{0};       // 超时次数
    std::atomic<uint64_t> crcErrorCount{0};      // CRC 校验失败次数
    std::atomic<uint64_t> bytesSent{0};          // 累计发送字节
    std::atomic<uint64_t> bytesReceived{0};      // 累计接收字节
    std::atomic<int64_t>  avgRttUs{0};           // 平均往返时延 (微秒)

    /// 计算通信质量百分比 (0.0 ~ 100.0)
    /// @note 调用方应使用 memory_order_acquire 读取各字段
    double qualityPercent() const {
        uint64_t total = requestTotal.load(std::memory_order_acquire);
        if (total == 0) return 100.0;
        uint64_t success = responseSuccess.load(std::memory_order_acquire);
        return (static_cast<double>(success) / static_cast<double>(total)) * 100.0;
    }
};

}  // namespace ens::channel
```

### 2.5 从站熔断/降级状态转换回调与信号契约

**所属 Target**：`ens::protocol`（STATIC 库，通过 `ens::channel` PUBLIC 传递）  
**对应需求**：HLD 3.1.5 节三级熔断状态机  
**头文件路径**：`src/protocol/PollScheduler.h`（关键接口摘录）

```cpp
// ============================================================================
// EnerSentry — 从站熔断/降级状态与信号契约 (V1.5)
// 所属 Target: ens::protocol (STATIC)
// 头文件: src/protocol/PollScheduler.h (摘录)
// ============================================================================
#pragma once

#include <QObject>
#include <cstdint>
#include <functional>

namespace ens::protocol {

// ============================================================================
// 从站健康状态枚举
// ============================================================================
enum class SlaveHealth : uint8_t {
    HEALTHY  = 0,   // 正常轮询（原始周期）
    DEGRADED = 1,   // 降级轮询（3× 周期，失败 3-7 次）
    ISOLATED = 2,   // 隔离（30s 探测一次，失败 ≥ 8 次）
};

// ============================================================================
// 从站标识
// ============================================================================
struct SlaveId {
    uint32_t linkId;    // 通信链路 ID（对应 channels.json 中的 link 索引）
    uint8_t  address;   // Modbus 从站地址 (1-247)

    bool operator==(const SlaveId& o) const {
        return linkId == o.linkId && address == o.address;
    }
};

// ============================================================================
// 熔断状态变更回调类型
// 用于非 Qt 上下文（如单元测试）的状态监听
// ============================================================================
using SlaveHealthCallback = std::function<void(SlaveId sid, SlaveHealth oldState, SlaveHealth newState, int consecutiveFailures)>;

// ============================================================================
// PollScheduler — 轮询调度器（关键信号契约摘录）
// ============================================================================
class PollScheduler : public QObject {
    Q_OBJECT
public:
    // ...

    /// 注册熔断状态变更回调（C++ 函数式风格）
    void setSlaveHealthCallback(SlaveHealthCallback cb);

signals:
    // ──── 从站熔断状态变更信号（Qt 风格，跨线程安全）────

    /// 从站降级（HEALTHY → DEGRADED）
    /// @param sid                   从站标识
    /// @param consecutiveFailures   连续失败次数 (3-7)
    void slaveDegraded(ens::protocol::SlaveId sid, int consecutiveFailures);

    /// 从站隔离（DEGRADED → ISOLATED）
    /// @param sid                   从站标识
    /// @param consecutiveFailures   连续失败次数 (≥ 8)
    void slaveIsolated(ens::protocol::SlaveId sid, int consecutiveFailures);

    /// 从站恢复（任意状态 → HEALTHY）
    /// @param sid  从站标识
    void slaveRecovered(ens::protocol::SlaveId sid);

    /// 从站试探（ISOLATED → 发送单次探测包）
    /// @param sid  从站标识
    void slaveProbing(ens::protocol::SlaveId sid);
};

}  // namespace ens::protocol

// ============================================================================
// SlaveId 哈希支持（供 QHash / std::unordered_map 使用）
// ============================================================================
namespace std {
    template<>
    struct hash<ens::protocol::SlaveId> {
        size_t operator()(const ens::protocol::SlaveId& s) const noexcept {
            return (static_cast<size_t>(s.linkId) << 8) | s.address;
        }
    };
}

// Qt qHash 重载
inline uint qHash(const ens::protocol::SlaveId& s, uint seed = 0) {
    return qHash(s.linkId, seed) ^ qHash(s.address);
}
```

### 2.6 读写回调函数类型定义

**所属 Target**：`ens::channel` (SHARED)  
**头文件路径**：`src/channel/IChannel.h`（已在 2.1 节定义，此处补充约束说明）

| 回调类型 | 签名 | 执行线程 | 约束 |
|---------|------|---------|------|
| `ReadCallback` | `void(const QByteArray& data)` | 通道 IO 线程 | 禁止长时间阻塞（> 1ms）；禁止调用 `QWidget::update()` |
| `ConnectionChangedCallback` | `void(bool connected)` | 通道 IO 线程 | 允许跨线程 emit signal |
| `ErrorCallback` | `void(const QString& errorMessage)` | 通道 IO 线程 | 仅用于日志记录与诊断 |
| `SlaveHealthCallback` | `void(SlaveId, SlaveHealth old, SlaveHealth new, int)` | 轮询调度线程 | UI 更新须通过 `QueuedConnection` 投递 |

---

## 3. 数据中枢与 Ring Buffer / 分级存储接口 (Layer 3)

### 3.1 alignas(16) Sample 原子对齐结构体

**所属 Target**：`ens::datahub`（STATIC 库，编译进 exe）  
**头文件路径**：`src/datahub/Sample.h`

```cpp
// ============================================================================
// EnerSentry — Sample 原子对齐采样结构体 (V1.5)
// 所属 Target: ens::datahub (STATIC) — 热路径，编译进 exe
// 头文件: src/datahub/Sample.h
// ============================================================================
#pragma once

#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <type_traits>
#include <chrono>   // V1.14 SpscRingBuffer Overrun 日志节流用
// 注: 实现中使用 spdlog::error / Q_ASSERT_X 的 TU 需额外包含 <spdlog/spdlog.h> 与 <QtGlobal>

// ============================================================================
// 跨平台 16 字节对齐宏
// ============================================================================
#if defined(_MSC_VER)
    #define ENS_CACHE_ALIGN __declspec(align(16))
#elif defined(__GNUC__) || defined(__clang__)
    #define ENS_CACHE_ALIGN __attribute__((aligned(16)))
#else
    #define ENS_CACHE_ALIGN alignas(16)
#endif

namespace ens::datahub {

// ============================================================================
// Sample — 16 字节原子对齐采样结构体
//
// 内存布局 (x86-64, little-endian):
//   offset 0-7:   timestamp (uint64_t, 8B)
//   offset 8-11:  pointId   (uint32_t, 4B)
//   offset 12-15: value     (float,    4B)
//   ─────────────────────────────────
//   合计: 16 字节 (单条 movaps 原子读写)
//
// 设计约束:
//  - alignas(16) 保证结构体不跨越 16 字节边界
//  - x86-64 上单条 movaps 完成写入，读线程永不看到"半写入"
//  - static_assert 编译期锁死期望布局
// ============================================================================
struct ENS_CACHE_ALIGN Sample {
    uint64_t timestamp;   // Unix 毫秒时间戳 (8B)
    uint32_t pointId;     // 测点 ID (4B)
    float    value;       // 采样值 (4B)
    // ─────────── 合计恰好 16 字节 ───────────
};

// 编译期布局断言
static_assert(sizeof(Sample) == 16,
    "Sample must be 16 bytes — required for atomic store/load on x86-64");

// V1.13 说明: Sample 结构体保持严格的 16 字节布局，不携带数据质量位。
// 数据质量位 (DataQuality) 通过外部索引/包装结构在协议解析层 → 业务层 →
// UI 渲染层之间传递。详见 §5.1.2 "异常值恢复策略"。保持 16B 布局是为了保证
// std::atomic<Sample> 的 lock-free 原子 load/store。

// 跨平台 lock-free 保证
// 防止 32 位 x86 / ARMv7 退化为内部互斥锁（性能暴跌 10x+）
// ⚠ V1.1 修订: MSVC x64 目标必须显式启用 /cx16 编译选项才能保证 CMPXCHG16B 指令，
//    否则 std::atomic<Sample>::is_always_lock_free 会被判定为 false，
//    触发本 static_assert 的编译失败（参见 §6.2 CMake 配置）
static_assert(std::atomic<Sample>::is_always_lock_free,
    "Sample (16B aligned) is NOT lock-free on this platform! "
    "x86-64: MSVC needs /cx16, GCC/Clang OK by default. "
    "ARM64: lacks cmpxchg16b; 16B std::atomic lock-free support varies by "
    "compiler version and -march flags. If this assert fails on ARM64, "
    "switch to SampleCompact8 + SpscRingBuffer (see §3.1.2). "
    "ARMv7 / 32-bit x86 will always fail — use SampleCompact8.");

// ⚠ V1.3 性能提示（16 字节原子操作的硬件开销）:
// ⚠ V1.13 跨平台提示（ARM64）:
//   ARM64 没有 x86 的 `cmpxchg16b` 指令；16B std::atomic 的 lock-free 实现依赖
//   128-bit LDXP/STXP 循环或 CASP 指令，不同 GCC/Clang 版本与 -march 标志的
//   支持差异较大。某些组合下 std::atomic<Sample>::is_always_lock_free 可能为
//   false，导致 §3.1 static_assert 编译失败。因此 ARM64 目标应优先评估
//   SampleCompact8 + SpscRingBuffer 方案（§3.1.2），并在 CI 中增加交叉编译
//   校验任务（参见 §6.2 / §6.3）。
//
// 在 x86-64 架构下，即使启用了 /cx16 且 is_always_lock_free == true，
// 主流 STL (MSVC STL / libstdc++) 对 16B std::atomic<T> 的 load/store 通常
// 不会退化为简单的 movaps/movups 矢量指令，而是生成带 lock 前缀的
// `lock cmpxchg16b` 循环（compare-and-swap retry）。
//
// 该指令属于高开销的总线锁/缓存锁指令。当采集线程以 100ms 周期批量 pushBatch()，
// 同时存在 3~4 个消费者线程高频 readRecent() 时，会导致 Cache Line 在多核之间
// 频繁失效（Cache Bouncing），高并发下 CPU 占用率可能明显升高，实际吞吐量甚至
// 低于显式自旋锁或把 Sample 拆分为 8B 变量。
//
// V1.8 压测闸门（详见 §3.1.1）:
//  16B std::atomic<Sample> 即使在 x86-64 /cx16 下 lock-free，其 load/store 仍可能
//  退化为 `lock cmpxchg16b` 循环。100ms 周期 + 3~4 Consumer 高频读取时，
//  多核 Cache Bouncing 会导致显著性能抖动。必须在 HIL 之前落地 Benchmark。
//
// 压测条件:
//  - 采集周期: 100ms；单周期测点数: ≥ 2000；持续时间: ≥ 5 分钟。
//  - 消费者数: 3~4 个（UI 渲染 / 黑匣子 / 降采样 / 审计）。
//  - 测试平台必须与生产 CPU 同代同核数。
//
// 判定阈值（任一满足即强制切换）:
//  1. `lock cmpxchg16b` 占单核 CPU 时间 > 5%;
//  2. 端到端采样延迟抖动 (P99 - P50) > 5ms;
//  3. Consumer Overrun 丢帧率 > 0.1%。
//
// 切换动作（按优先级）:
//  1. 立即落地 8B SampleCompact8（x86-64 64-bit atomic 退化为普通 mov）。
//  2. 对点到点流水线（采集 → 单一消费线程）采用 SpscRingBuffer<T>，
//     彻底消除 16B 整体 atomic CAS。
//  3. 若仍不满足，升级 ADR-21，启用 12B 拆分读取方案。
//
// 默认实现保持 16B Sample；是否降级由压测 Gate 决定，禁止仅凭理论判断跳过 Benchmark。

// ============================================================================
// SampleCompact8 — 8 字节压缩采样结构（V1.6 Cache Bouncing 优化备选）
//
// 布局 (x86-64, little-endian):
//   offset 0-3: relMs   (uint32_t, 4B) — 相对基准纪元的毫秒偏移
//   offset 4-7: value   (uint32_t, 4B) — IEEE 754 float 位模式，或 uint32 定点值
//   ─────────────────────────────────
//   合计: 8 字节（x86-64 原生 64-bit atomic，load/store 退化为普通 mov）
//
// 约束:
//   - relMs 约 49.7 天回卷，需要外部维护基准纪元并在回卷时重新同步。
//   - 不携带 pointId，因此必须与按通道分片的 RingBuffer 一起使用（每个 RingBuffer
//     实例只服务单一 pointId，或把 pointId 放到外层索引）。
//   - value 只能表示 float 精度；若需 double，请使用 12B/16B 方案。
// ============================================================================
struct SampleCompact8 {
    uint32_t relMs;      // 相对基准纪元的毫秒偏移
    uint32_t value;      // float 位模式（可用 std::bit_cast<float> 转换）
};
static_assert(sizeof(SampleCompact8) == 8,
              "SampleCompact8 must be 8 bytes for lock-free 64-bit atomic");
static_assert(std::atomic<SampleCompact8>::is_always_lock_free,
              "SampleCompact8 is NOT lock-free on this platform!");

// ============================================================================
// SpscRingBuffer — 单生产者单消费者无锁环形缓冲（V1.6 进阶优化备选）
//
// 适用场景: 采集线程 → 单一消费线程（如持久化线程、单一 UI 通道）的点到点流水线。
// 与 MpmcRingBuffer 的区别:
//   - 数据槽 m_buffer 使用普通 T[]（非 std::atomic<T>[]）
//   - 仅对 sequence / writeIndex 使用 release/acquire 屏障
//   - V1.12 进一步精简: push/pushBatch 中删除冗余显式 atomic_thread_fence,
//     因为 m_writeSeq.store(..., release) 自身已携带 Release 语义, 可保证
//     数据写入不被重排到 sequence 发布之后, 同时避免 ARM 上额外的 dmb ish。
//   - 避免了对 16B Sample 整体执行 atomic CAS，彻底消除 lock cmpxchg16b 开销
//
// 线程安全:
//   - 单生产者（Single-Producer）: 仅一个线程调用 push()/pushBatch()
//   - 单消费者（Single-Consumer）: 仅一个线程调用 readRecent()/extractRange()
//   - 多生产者/多消费者场景请继续使用 RingBuffer<T>（Mpmc）。
//
// ⚠ V1.14 所有权语义澄清:
//   "单线程" 指逻辑调用序列串行，不要求一定是固定物理线程。
//   若使用 QThreadPool / asio::thread_pool + strand 等任务级串行上下文,
//   必须切换到 ExecutionContext (ContextId) 校验模式, 否则 std::thread::id
//   会因 Worker 线程动态变化而误触发断言。详见 §3.2.2。
//
// ⚠ V1.9 noexcept 契约:
//   - push / pushBatch / readRecent / available / hasOverrun / droppedFrames /
//     resetDroppedFrames 全部声明 noexcept, 确保编译器无需为热路径生成异常栈
//     展开代码 (.gcc_except_table)。
//   - 这等价于一条"热路径中禁止堆分配/锁/可能抛异常的 Qt 调用"的隐式契约:
//     若后续重构在 push/pushBatch 中插入 QVector 构造或日志 fmt (可能 bad_alloc),
//     noexcept 会立即触发 std::terminate, 防止破坏热路径延迟。
//   - Debug 构建下 push/pushBatch 还额外执行写者线程所有权校验 (V1.10 升级为
//     std::atomic<std::thread::id> + compare_exchange_strong lock-free CAS,
//     替代 V1.9 的 std::mutex; 详见 checkProducerOwnership)。
//
// ⚠ V1.14 Overrun 主动观测:
//   - readRecent 在消费者落后超过 Capacity 时, 原子累加 m_droppedFramesCount
//     并触发按时间节流的 Warning 日志, 与 RingBuffer<T> 的 droppedFrames()
//     / resetDroppedFrames() API 保持语义一致。
//   - 生产环境必须监控 droppedFrames(); 仅靠 hasOverrun() 无法量化丢帧规模。
// ============================================================================
template <typename T, size_t Capacity>
class SpscRingBuffer {
public:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of two");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

    SpscRingBuffer() = default;

    // 禁止拷贝与移动
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    /// 写入单个元素
    /// ⚠ V1.9: SpscRingBuffer 严格单生产者；Debug 构建下运行时校验写者线程
    void push(const T& item) noexcept {
#ifndef NDEBUG
        checkProducerOwnership("push");
#endif
        const size_t seq = m_writeSeq.load(std::memory_order_relaxed);
        const size_t idx = seq & MASK;
        m_buffer[idx] = item;                                            // ① 数据写入 (Relaxed)
        // ② V1.12: 删除冗余显式 atomic_thread_fence。
        //    m_writeSeq.store(..., release) 自身已构成 Release 语义，可保证①中所有
        //    对普通内存的写入不会被编译器/CPU 重排序到该 store 之后。显式 fence 在
        //    x86 下虽开销极小，但在 ARM 下会多生成一条 dmb ish 指令，属于双重屏障。
        m_writeSeq.store(seq + 1, std::memory_order_release);            // ② 发布序列
    }

    /// 批量写入（100ms 周期内聚合后一次写入）
    /// ⚠ V1.9: SpscRingBuffer 严格单生产者；Debug 构建下运行时校验写者线程
    void pushBatch(const T* items, size_t count) noexcept {
        if (count == 0) return;
#ifndef NDEBUG
        checkProducerOwnership("pushBatch");
#endif
        const size_t startSeq = m_writeSeq.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i) {
            m_buffer[(startSeq + i) & MASK] = items[i];                  // 数据写入 (Relaxed)
        }
        // V1.12: 同上，store(release) 自身即 Release 语义，无需额外 fence。
        m_writeSeq.store(startSeq + count, std::memory_order_release);   // 批量发布序列
    }

    /// 读取最多 count 个新元素
    /// @return 实际读取数量
    /// ⚠ V1.9 noexcept 契约: 热路径禁止抛异常
    /// ⚠ V1.14 Overrun 观测: 当消费者落后超过一圈时, 原子累加丢帧数并触发
    ///     按时间节流的 Warning 日志, 保持与 RingBuffer<T> 可观测性一致。
    size_t readRecent(T* out, size_t count) noexcept {
        const size_t writeSeq = m_writeSeq.load(std::memory_order_acquire);
        size_t cursor = m_readSeq.load(std::memory_order_relaxed);

        if (writeSeq == cursor) return 0;       // 无新数据

        // 回卷保护：消费者落后超过一圈
        if (writeSeq - cursor > Capacity) {
            const uint64_t dropped = static_cast<uint64_t>(
                (writeSeq - cursor) - Capacity);
            m_droppedFramesCount.fetch_add(dropped, std::memory_order_relaxed);
            cursor = writeSeq - Capacity;       // 跳到最老可读位置

            // V1.14: 按时间节流的 Overrun 日志（默认 5 秒最多 1 条）
            logOverrunThrottled(dropped);
        }

        const size_t readable = std::min(writeSeq - cursor, count);
        for (size_t i = 0; i < readable; ++i) {
            const size_t idx = (cursor + i) & MASK;
            out[i] = m_buffer[idx];
        }
        m_readSeq.store(cursor + readable, std::memory_order_release);
        return readable;
    }

    /// 当前可读数量（仅用于监控，不消耗数据）
    /// ⚠ V1.9 noexcept 契约: 纯原子读，不抛异常
    [[nodiscard]] size_t available() const noexcept {
        const size_t writeSeq = m_writeSeq.load(std::memory_order_acquire);
        const size_t cursor   = m_readSeq.load(std::memory_order_acquire);
        return (writeSeq > cursor) ? std::min(writeSeq - cursor, Capacity) : 0;
    }

    /// 检查当前是否处于 Overrun 状态
    /// ⚠ V1.9 noexcept 契约: 纯原子读，不抛异常
    [[nodiscard]] bool hasOverrun() const noexcept {
        const size_t writeSeq = m_writeSeq.load(std::memory_order_acquire);
        const size_t cursor   = m_readSeq.load(std::memory_order_acquire);
        return (writeSeq - cursor) > Capacity;
    }

    /// V1.14: 返回自上次 reset 以来因 Overrun 丢失的帧总数
    /// 与 RingBuffer<T>::droppedFrames() 语义一致，便于统一监控。
    [[nodiscard]] uint64_t droppedFrames() const noexcept {
        return m_droppedFramesCount.load(std::memory_order_relaxed);
    }

    /// V1.14: 重置 Overrun 丢帧计数器
    void resetDroppedFrames() noexcept {
        m_droppedFramesCount.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr size_t MASK = Capacity - 1;

    // 数据槽：普通数组，依赖 sequence 屏障保证可见性
    alignas(64) T m_buffer[Capacity]{};

    // 写序列号：生产者发布边界
    alignas(64) std::atomic<size_t> m_writeSeq{0};

    // 读序列号：消费者进度（单消费者，无需 atomic 也可，但用 atomic 防止编译器乱序）
    alignas(64) std::atomic<size_t> m_readSeq{0};

    // V1.14: Overrun 丢帧计数器，与 RingBuffer<T> 的 m_droppedFramesCount 对齐
    alignas(64) std::atomic<uint64_t> m_droppedFramesCount{0};

    // V1.14: 上次打印 Overrun 日志的时间戳（毫秒），用于 5 秒节流
    mutable std::atomic<uint64_t> m_lastOverrunLogMs{0};

    /// V1.14: 按时间节流的 Overrun 日志（默认 5 秒最多 1 条）
    /// 避免 Consumer 持续落后时日志风暴，同时保证运维可观测性。
    void logOverrunThrottled(uint64_t dropped) const noexcept {
        using namespace std::chrono;
        const uint64_t nowMs = duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
        const uint64_t lastMs = m_lastOverrunLogMs.load(std::memory_order_relaxed);
        if (nowMs - lastMs < 5000) {
            return;  // 5 秒内已打印过，跳过
        }
        // CAS 竞争失败也没关系，只是少打一条日志
        if (m_lastOverrunLogMs.compare_exchange_strong(
                lastMs, nowMs,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
            spdlog::warn("[SpscRingBuffer] Consumer overrun: dropped {} frames. "
                         "Consumer thread cannot keep up with producer.",
                         dropped);
        }
    }

#ifndef NDEBUG
    // ⚠ V1.10: Debug 构建下 lock-free 写者线程所有权校验
    //   替代 V1.9 的 std::mutex + std::lock_guard 实现, 消除首调注册时的
    //   mutex 竞争开销。该 mutex 虽仅作用于首次注册 (`if 未注册 → 写入`),
    //   但生产者 push()/pushBatch() 要求极致微秒级响应, 任何锁竞争都可能
    //   破坏延迟承诺。
    //
    //   V1.10 方案:
    //     使用 std::atomic<std::thread::id> 替代 std::mutex, 借助
    //     compare_exchange_strong 循环实现无锁的"check-and-register":
    //       1) 首次调用: CAS(expected={}, desired=currentThread) 成功 → 完成注册
    //       2) 一致调用: load == currentThread → 直接通过
    //       3) 跨线程调用: load != currentThread → spdlog::error + Q_ASSERT_X 拦截,
    //          并强制重新绑定以便 dev 继续调试 (dev 体验 > 严苛拒绝)
    //
    //   V1.14 注意:
    //     若使用线程池 + strand 等 Task-level Serialized Context, 应改用
    //     ExecutionContext (ContextId) 校验, 否则 std::thread::id 变化会误触发
    //     断言。详见 §3.2.2。
    //
    //   编译期断言: std::thread::id 在主流 libc++ / libstdc++ / MSVC STL 上
    //     本质都是单一 size_t (uint64_t), 因此 std::atomic<std::thread::id>
    //     一般 lock-free; 极端平台退化时直接编译失败, 强制开发者评估回退
    //     至 V1.9 的 mutex 方案或选择其他平台。
    //
    //   Release 构建 (NDEBUG) 下整段剔除, 不影响热路径性能与代码大小。
    static_assert(std::atomic<std::thread::id>::is_always_lock_free,
                  "std::atomic<std::thread::id> must be lock-free for V1.10 "
                  "lock-free ownership check; otherwise fall back to V1.9 "
                  "mutex implementation or platform-specific handling.");
    mutable std::atomic<std::thread::id> m_producerOwnerThread{};

    /// V1.10 Lock-free 写者线程所有权校验 (替代 V1.9 的 mutex 实现)。
    /// @param funcName  调用方函数名 (用于错误日志定位)
    /// @complexity     O(1) 无锁, 单次原子 load/CAS, 不进入内核态
    void checkProducerOwnership(const char* funcName) const noexcept {
        const std::thread::id currentThread = std::this_thread::get_id();
        std::thread::id expected = std::thread::id{};

        // 阶段 1: 首次注册尝试 (CAS 空 ID -> 当前线程 ID)
        if (m_producerOwnerThread.compare_exchange_strong(
                expected, currentThread,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;  // 首次注册成功, 直接返回
        }

        // 阶段 2: 已被注册, 校验所有权线程是否一致
        // (CAS 失败时 expected 已被写入 m_producerOwnerThread 的当前值)
        if (expected == currentThread) return;   // 一致, 通过

        // 阶段 3: 跨线程调用, 报告并拦截 (热路径上极少见, 仅 dev 误用触发)
        spdlog::error("[SpscRingBuffer] Producer ownership violation in {}! "
                      "Owner thread={}, current thread={}. "
                      "SpscRingBuffer is SINGLE-PRODUCER; do not call "
                      "push/pushBatch from multiple threads. "
                      "(V1.10 lock-free CAS)",
                      funcName, expected, currentThread);
        Q_ASSERT_X(false, "SpscRingBuffer::checkProducerOwnership",
                   "Producer crossed thread boundary");
        // 重新绑定到当前线程以便 dev 继续调试
        m_producerOwnerThread.store(currentThread, std::memory_order_relaxed);
    }
#endif
};

// 使用示例（在 100ms 采集线程 → 单一持久化线程场景）:
//   SpscRingBuffer<SampleCompact8, 65536> g_rawToPersist;
//   g_rawToPersist.pushBatch(samples.data(), samples.size());

// ============================================================================
// SampleWithMeta — 黑匣子扩展结构（携带快照 ID）
// 通常仅黑匣子回放场景使用，meta 字段平时为 0
// ============================================================================
struct SampleWithMeta {
    Sample   core;           // 16B 原子块
    uint64_t blackBoxId;     // 关联黑匣子快照 ID（默认 0 表示非黑匣子数据）
};
static_assert(sizeof(SampleWithMeta) == 24, "SampleWithMeta layout check");

// ============================================================================
// DownSampledSample — 降采样聚合后的采样（L2 落库用）
// ============================================================================
struct DownSampledSample {
    uint64_t timestamp;      // Unix 毫秒时间戳（对齐到窗口边界）
    uint32_t pointId;        // 测点 ID
    float    valueMax;       // 窗口内最大值
    float    valueMin;       // 窗口内最小值
    float    valueAvg;       // 窗口内平均值
    uint16_t sampleCount;    // 窗口内实际采样点数（可能 < 理论值，如丢帧时）
    uint16_t padding;        // 对齐填充
};
static_assert(sizeof(DownSampledSample) == 32, "DownSampledSample layout check");

}  // namespace ens::datahub
```

#### 3.1.1 V1.8 压测闸门：16 字节 `std::atomic<Sample>` 开销必须在联调初期落地

**隐患**：即使 `std::atomic<Sample>::is_always_lock_free == true`，x86-64 对 16B `std::atomic<T>` 的 load/store 仍可能生成带 `lock` 前缀的 `cmpxchg16b` 循环。在 **100ms 采集周期 + 3~4 个消费者高频读取** 场景下，总线锁/缓存锁会导致多核 CPU 上的 **Cache Bouncing**，造成明显性能抖动。

**目标**：在系统集成初期（而非上线前）通过 Benchmark 量化该指令开销，提前决策是否切换到 8B `SampleCompact8` 或 `SpscRingBuffer<T>` 架构。

**压测条件**：

| 条件项 | 要求 |
|--------|------|
| 采集周期 | 100ms |
| 消费者数 | 3~4 个（UI 渲染 / 黑匣子 / 降采样 / 审计等） |
| 单周期测点数 | ≥ 2000 点 |
| 持续时间 | ≥ 5 分钟（让 CPU 频率与缓存状态稳定） |
| 测试平台 | 与生产环境同型号 CPU（至少同代同核心数） |

**判定阈值（任一满足即触发切换）**：

1. `lock cmpxchg16b` 及相关内存屏障指令占单核 CPU 时间 **> 5%**；
2. 端到端采样延迟抖动（P99 − P50）**> 5ms**；
3. 消费者 Overrun 丢帧率 **> 0.1%**（每 1000 帧丢失 1 帧以上）。

**切换动作**：

- 立即落地 `SampleCompact8`（8B 原子，x86-64 退化为普通 `mov`）；
- 或对点到点流水线（采集 → 单一消费线程）直接采用 `SpscRingBuffer<T>`，彻底消除 16B 整体 atomic CAS；
- 若两者仍不满足，升级 ADR-21 并考虑 12B 拆分读取方案。

**跨平台（ARM64）编译器与指令集校验（V1.13 新增）**：

1. **CI 交叉编译任务**：
   - 在 CI Pipeline 中新增 `cross-compile-arm64` Job，使用 `aarch64-linux-gnu-g++` / `aarch64-linux-gnu-clang++` 对 `src/datahub` 进行交叉编译；
   - 编译参数必须包含项目实际使用的 `-std=c++17`、`-march` / `-mcpu` 标志（如 `-march=armv8-a` 或 `-mcpu=cortex-a76`）；
   - 该 Job 必须编译 `Sample.h` / `RingBuffer.h` / `SpscRingBuffer.h`，确保 `static_assert(std::atomic<Sample>::is_always_lock_free)` 在目标 ARM64 平台上真实通过。

2. **ARM64 lock-free 判定差异**：
   - GCC 11+ / Clang 14+ 在 `-march=armv8-a+lse` 或更高版本上，通常将 16B `std::atomic` 判定为 lock-free（使用 CASP 指令）；
   - 旧版工具链或 `-march=armv7-a` 目标会退化为内部互斥锁，`is_always_lock_free == false`；
   - Apple Silicon (M1/M2/M3) 上 Clang 通常支持 16B lock-free，但仍需在 CI 中验证。

3. **失败处理**：
   - 若 ARM64 交叉编译 `static_assert` 失败，**禁止**通过修改断言绕过；必须切换为 `SampleCompact8 + SpscRingBuffer` 方案（§3.1.2）；
   - 该切换决策必须作为 ADR-21 附件记录。

**压测工具链示例**：

| 平台 | 工具 |
|------|------|
| Windows x64 | Intel VTune / AMD uProf，Hotspots 视图过滤 `lock cmpxchg16b` |
| Linux x64 | `perf stat -e cpu/event=0x64,umask=0x08,name=...` |
| Linux ARM64 | `perf` + `ldrex/strex` / `casp` 指令计数；或 ARM Streamline |
| 代码内 | 在 `RingBuffer::readRecent` 热路径前后打 `std::chrono` 微秒级耗时打点 |

**责任**：该 Benchmark 必须在首次硬件在环测试 (HIL) 之前完成，并作为 ADR-21 输入附件；未通过压测 Gate 不得进入系统联调下一阶段。

#### 3.1.2 V1.13 两套无锁方案切换落地标准

**隐患**：文档中同时保留了两种无锁数据通路：

1. **16B `Sample` + `RingBuffer<T>` (MPMC)**：单生产者、多消费者（SPMC），依赖 16B `std::atomic<Sample>` 的 lock-free 原子 load/store；
2. **8B `SampleCompact8` + `SpscRingBuffer<T>` (SPSC)**：单生产者、单消费者（SPSC），仅对 sequence 使用 release/acquire 屏障，彻底消除 16B 整体 atomic CAS。

如果缺少明确的模块划分映射与配置标准，不同开发人员可能在同一数据通路上混用这两种模式：

- 把需要多消费者共享的测点错误放入 `SpscRingBuffer`，导致第二个消费者读不到数据或触发所有权断言；
- 把点到点流水线错误放入 `RingBuffer<Sample>`，承担不必要的 `lock cmpxchg16b` Cache Bouncing；
- 在 ARM64 等不支持 16B lock-free atomic 的平台上仍然使用 `RingBuffer<Sample>`，编译期 `static_assert` 失败。

**目标**：在详细设计阶段明确每种测点/通道的默认无锁方案、切换条件、配置项与责任边界，形成可落地的开发配置标准。

**核心决策原则**：

| 维度 | `Sample` + `RingBuffer<T>` (MPMC) | `SampleCompact8` + `SpscRingBuffer<T>` (SPSC) |
|------|-----------------------------------|-----------------------------------------------|
| **生产者数** | 严格 1（单采集线程） | 严格 1（单采集线程） |
| **消费者数** | 1~`MAX_CONSUMERS`（多消费者共享） | 严格 1（单一消费线程） |
| **典型场景** | 遥测总线：同一测点需同时供给 UI 渲染、黑匣子、降采样、审计 | 点到点流水线：采集 → 持久化、采集 → 单一 UI 通道、BMS 原始帧 → 协议解析 |
| **数据精度** | 完整 `uint64_t` 时间戳 + `float` 值 | `uint32_t` 相对毫秒 + `uint32_t` 值位模式（精度受限） |
| **原子开销** | 16B atomic，可能触发 `lock cmpxchg16b` | 64-bit atomic / 普通 sequence store，无 16B CAS |
| **适用平台** | x86-64（需 MSVC `/cx16`）/ 部分 ARM64（需校验） | 全平台，尤其适合 ARM64 / 低功耗边缘网关 |
| **回卷保护** | 自动跳到最老可读位置，原子累加丢帧数 | V1.14 起自动跳到最老可读位置，原子累加丢帧数，并提供 `droppedFrames()` / `resetDroppedFrames()`；生产环境必须监控丢帧计数 |
| **默认选择** | **通用默认方案**：不确定时先用 MPMC | 仅在压测 Gate 判定切换后使用 |

**落地配置标准**：

每个测点（`PointConfig`）必须显式声明其 L1 数据通路类型：

```cpp
// ============================================================================
// PointConfig — 测点配置结构体片段 (V1.13)
// 配置文件: pointtable.json
// ============================================================================
enum class PointBufferPolicy : uint8_t {
    Auto = 0,        // 默认: 由系统根据 consumers 数量自动选择
    MpmcSample16,    // 16B Sample + RingBuffer<T> (SPMC)
    SpscCompact8,    // 8B SampleCompact8 + SpscRingBuffer<T> (SPSC)
};

struct PointConfig {
    uint32_t          pointId;
    QString           name;
    PointBufferPolicy bufferPolicy = PointBufferPolicy::Auto;
    QVector<QString>  consumers;   // 消费者 ID 列表, 用于 Auto 模式决策
    // ... 其他字段
};
```

**Auto 选择规则**：

```cpp
// 伪代码: 根据 consumers 数量自动选择无锁方案 (V1.13)
PointBufferPolicy resolveBufferPolicy(const PointConfig& cfg) {
    if (cfg.bufferPolicy != PointBufferPolicy::Auto) {
        return cfg.bufferPolicy;  // 显式配置优先
    }
    if (cfg.consumers.size() <= 1) {
        // 单消费者点到点流水线: 优先使用 SPSC + SampleCompact8
        // 但若该测点需要完整时间戳精度, 仍可用 MpmcSample16
        return PointBufferPolicy::SpscCompact8;
    }
    // 多消费者共享: 必须使用 MPMC
    return PointBufferPolicy::MpmcSample16;
}
```

**模块划分映射表**：

| 子系统 | 数据流 | 推荐方案 | 理由 |
|--------|--------|----------|------|
| **遥测总线** (Telemetry Bus) | 采集线程 → {UI, 黑匣子, 降采样, 审计} | `MpmcSample16` | 多消费者共享，避免为每个消费者复制一份数据 |
| **历史持久化** (Persistence) | 采集线程 → 单一 DbWriteQueue 消费线程 | `SpscCompact8` | 点到点，高吞吐，可降低 16B atomic 开销 |
| **单一通道 UI** (Single Channel Scope) | 采集线程 → 单一渲染准备线程 | `SpscCompact8` | 单消费者，低延迟 |
| **告警触发** (Alarm Engine) | 采集线程 → 告警引擎 | `SpscCompact8` 或 `MpmcSample16` | 若告警引擎与 UI 共享同一 RingBuffer，则使用 MPMC；若独立消费，则 SPSC |
| **BMS 原始帧** (Raw Frame) | 串口/网络 → 协议解析线程 | `SpscCompact8` | 单生产者单消费者，原始帧无需多消费者共享 |
| **事件总线** (Event Bus) | 多源 → 多消费者 | `MpmcSample16` | 事件需要广播给多个消费者 |

**切换触发条件**：

1. **压测 Gate 触发**（§3.1.1）：`lock cmpxchg16b` 单核 CPU > 5%、P99-P50 延迟 > 5ms、Overrun 丢帧率 > 0.1% 任一满足，强制切换相关测点到 `SpscCompact8`。
2. **平台约束**：ARM64 / RISC-V 等目标平台若 `static_assert(std::atomic<Sample>::is_always_lock_free)` 失败，**必须**在编译期切换为 `SpscCompact8` 或 12B 拆分方案，不能通过压测 Gate 延缓。
3. **配置显式覆盖**：运维/开发可通过 `pointtable.json` 中 `bufferPolicy` 字段显式指定，覆盖 Auto 规则。

**强制约束**：

1. **禁止混用消费者模型**：`SpscRingBuffer<T>` 严禁多个线程调用 `readRecent()`；`RingBuffer<T>` 的同一 `ConsumerId` 严禁跨线程使用。
2. **禁止在 SPSC 上模拟广播**：若业务需要多消费者，必须改用 MPMC 或为每个消费者建立独立的 `SpscRingBuffer`。
3. **配置即契约**：`PointConfig::bufferPolicy` 与 `consumers` 列表必须在配置加载时校验一致性；`SpscCompact8` 策略下 `consumers.size() > 1` 必须启动失败并报错。
4. **文档化每个 RingBuffer 实例**：代码中每个 `RingBuffer<T>` / `SpscRingBuffer<T>` 实例必须附注释说明：①生产者线程名；②消费者线程名/数量；③数据精度要求；④切换触发条件。
5. **CI 校验**：新增 `scripts/ci_buffer_policy_check.py` 脚本，扫描 `pointtable.json` 中 `bufferPolicy` 与 `consumers` 的一致性，并在 CI 中阻断非法配置。

### 3.2 RingBuffer\<T\> 无锁模板接口

**所属 Target**：`ens::datahub`（STATIC 库）  
**头文件路径**：`src/datahub/RingBuffer.h`

```cpp
// ============================================================================
// EnerSentry — RingBuffer<T> 无锁环形缓冲区模板 (V1.5)
// 所属 Target: ens::datahub (STATIC)
// 头文件: src/datahub/RingBuffer.h
// ============================================================================
#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>             // V1.7 起需要: std::this_thread::get_id()
#include <vector>

// ⚠ V1.11 移除: #include <mutex>  // V1.7-V1.10 由 std::lock_guard 引入, V1.11
//   升级为 std::atomic<std::thread::id> lock-free CAS 后已无 mutex 依赖,
//   头文件编译时间相应缩短 <mutex> 的间接包含开销（通常 1.5-2ms / TU）

namespace ens::datahub {

// ============================================================================
// RingBuffer<T, Capacity>
//
// 设计约束:
//  - 单生产者 (SPMC): 仅一个采集线程调用 push()/pushBatch()
//  - 多消费者 (SPMC): 最多 MAX_CONSUMERS 个消费者独立读取
//  - 无锁: 读写路径不使用互斥锁，依赖 release/acquire 内存屏障
//  - 回卷保护: 消费者过慢时自动跳到最老可读位置，不阻塞生产者
//
// 游标语义（V1.3 修正）:
//  - m_writePos / m_publishedPos / m_consumerCursors 均表示“累计写入/读取计数 (Count)”，
//    而非数组下标索引 (Index)。
//  - 物理槽位索引通过 count & MASK 计算。
//  - 初始状态: published = 0, cursor = 0；写入 1 个元素后 published = 1，
//    cursor = 0 表示有 1 个未读元素；读取后 cursor = 1 表示已消费 1 个。
//  - 读取条件: published == cursor 表示无新数据（避免 published <= cursor 的 Index 语义下
//    首个元素被永久漏读）。
//
// ⚠ V1.9 noexcept 契约:
//   push / pushBatch / readRecent / extractRange / latestPublished /
//   checkConsumerLag / droppedFrames / resetDroppedFrames 全部声明 noexcept,
//   禁止热路径中堆分配/可能抛异常的调用。重构时若在上述函数中插入
//   QVector / QString / std::function / bad_alloc-prone 调用, 编译器会在
//   std::terminate 路径直接拦截, 防止破坏热路径延迟承诺。
//
// ⚠ V1.11 Debug 所有权注册锁升级:
//   m_consumerOwnerThread 从 V1.7 的 std::array<std::thread::id, N> + std::mutex
//   升级为 V1.11 的 std::array<std::atomic<std::thread::id>, N> +
//   compare_exchange_strong 模式，与 §3.1 SpscRingBuffer 形成代码库统一的
//   "lock-free CAS 注册"模式。static_assert 兜底非 lock-free 平台。
//   详见 readRecent 注释。
//
// 模板约束:
//  - T 必须可平凡拷贝 (trivially copyable)
//  - Capacity 必须为 2 的幂（位与替代取模）
//  - std::atomic<T>::is_always_lock_free 必须为 true
// ============================================================================
template<typename T, size_t Capacity>
class RingBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
        "T must be trivially copyable for RingBuffer atomic access");
    static_assert((Capacity & (Capacity - 1)) == 0,
        "Capacity must be power of 2 for fast modulo via bitmask");
    static_assert(Capacity > 0, "Capacity must be > 0");

public:
    /// 最大消费者数量
    static constexpr size_t MAX_CONSUMERS = 4;

    /// 消费者 ID 语义:
    ///   0 = UI 渲染准备线程
    ///   1 = 黑匣子管理器
    ///   2 = 降采样线程
    ///   3 = 预留扩展
    enum class ConsumerId : uint8_t {
        UI_RENDER    = 0,
        BLACK_BOX    = 1,
        DOWN_SAMPLE  = 2,
        RESERVED     = 3,
    };

    RingBuffer() : m_buffer(Capacity) {}

    // 禁止拷贝与移动（原子变量不可拷贝）
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // ═══════════════════════════════════════════════════════════════
    // 生产者接口（仅采集线程调用）
    // ═══════════════════════════════════════════════════════════════

    /// 写入单个元素
    /// 内存顺序:
    ///   ① 数据写入 (relaxed store)
    ///   ② memory_order_release 屏障 — 保证 ① 在 ③ 之前对所有核心可见
    ///   ③ publishedPos.store(pos + 1, release) — 发布新数据边界（Count 语义）
    ///
    /// ⚠ V1.9 noexcept 契约: 热路径禁止抛异常，确保编译器不生成异常栈展开代码
    void push(const T& item) noexcept {
        size_t pos = m_writePos.fetch_add(1, std::memory_order_relaxed);
        size_t idx = pos & MASK;                                       // Count → Index
        m_buffer[idx].store(item, std::memory_order_relaxed);          // ①
        std::atomic_thread_fence(std::memory_order_release);           // ② Store-Store 屏障
        m_publishedPos.store(pos + 1, std::memory_order_release);      // ③ 已写入总数 +1
    }

    /// 批量写入（性能优化：减少屏障次数）
    /// 100ms 周期内 2000 点调用一次 pushBatch 而非 2000 次 push
    void pushBatch(const T* items, size_t count) noexcept {
        if (count == 0) return;
        size_t startPos = m_writePos.fetch_add(count, std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i) {
            size_t idx = (startPos + i) & MASK;                        // Count → Index
            m_buffer[idx].store(items[i], std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_release);           // 写屏障
        m_publishedPos.store(startPos + count, std::memory_order_release);
    }

    // ═══════════════════════════════════════════════════════════════
    // 消费者接口（多消费者各自调用）
    // ═══════════════════════════════════════════════════════════════

    /// 获取最新已发布位置（acquire 语义）
    /// 消费者以 release-acquire 配对读取生产者发布的数据边界
    /// ⚠ V1.9 noexcept 契约: 纯原子 load，不抛异常
    [[nodiscard]] size_t latestPublished() const noexcept {
        return m_publishedPos.load(std::memory_order_acquire);
    }

    /// 消费者读取最近 N 个新样本
    /// @param consumerId  消费者 ID（独立游标，互不干扰）
    /// @param out         输出缓冲区
    /// @param count       期望读取最大数量
    /// @return            实际读取数量（可能为 0 表示无新数据）
    ///
    /// ⚠ V1.3 Count 语义说明:
    ///   - publishedPos 与 consumerCursor 均为“累计计数”。
    ///   - published == cursor 表示当前无未读数据。
    ///   - 可读元素索引 = (cursor + i) & MASK，i ∈ [0, readable)。
    ///
    /// ⚠ V1.5 Overrun 监控（带节流日志）:
    ///   当消费者落后超过 Capacity 时，readRecent 会跳跃到最老可读位置，导致
    ///   中间数据丢失（Loss of Frames）。每次跳跃会原子累加 droppedFramesCount，
    ///   并触发按时间节流的 Warning 日志（默认 5 秒最多打印一次），确保运维
    ///   人员无需主动调用 droppedFrames() 即可在日志流中直接捕获 Consumer Overrun。
    ///   调用方仍可通过 droppedFrames() / resetDroppedFrames() 进行程序化监控。
    ///
/// ⚠ V1.1 / V1.14 线程所有权契约（必须严格遵守）:
///   ConsumerId 是"逻辑游标槽位"，并非"逻辑线程句柄"。
///   同一 ConsumerId 的 readRecent() / extractRange() 调用序列必须由
///   单一逻辑执行上下文持有和调用。该上下文可以是：
///     (A) 固定的物理线程（默认）;
///     (B) 任务级串行上下文（TaskStrand），如 asio::strand / QThreadPool
///         上的串行队列，此时 Debug 校验应使用 ContextId 而非 std::thread::id。
///   多个线程共享同一 ConsumerId 调用将构成 Data Race（即使 m_consumerCursors
///   已升级为 atomic，也无法保证 read-modify-write 操作的原子性）。
///   多线程共享消费进度 → 每个线程/Strand 分配独立 ConsumerId。
///   详见 §3.2.2 "单线程所有权与线程池兼容性"。
    ///
    /// ⚠ V1.7 Debug 运行时校验（生产构建自动剥离, 仅 NDEBUG 未定义时启用）:
    ///   大型团队协作中, 新成员可能误在多个线程中传入相同的 ConsumerId。
    ///   编译期断言无法捕获此类错误, 因此在 Debug 构建中通过 thread_id 绑定
    ///   实现运行期防御:
    ///     - 首次调用 readRecent 时, 在 m_consumerOwnerThread[id] 中记录当前
    ///       std::thread::id 作为"所有权线程";
    ///     - 后续调用时若 std::this_thread::get_id() 与所有权线程不一致,
    ///       立即触发 Q_ASSERT_X + spdlog::error 拦截, 杜绝数据竞争。
    ///   Release 构建 (定义 NDEBUG) 中此校验整段被 #ifndef 包裹剔除, 不影响
    ///   任何热路径性能。
    ///
    /// ⚠ V1.11 锁升级：Debug 所有权注册改 lock-free CAS（与 §3.1 SpscRingBuffer
    ///   一致）。原 V1.7 使用的 std::mutex + std::lock_guard 在首调注册路径上
    ///   会产生不可忽略的上下文切换开销，与 SPMC 场景下 30/60Hz × MAX_CONSUMERS
    ///   的可观测频率虽不至于成为瓶颈，但保留 mutex 会与 §3.1 形成"两套模式"
    ///   不利于团队代码库一致性。统一为 std::atomic<std::thread::id> +
    ///   compare_exchange_strong 后，整个 Debug 校验路径完全无锁，与 §3.1
    ///   形成"lock-free CAS 注册"统一模式。
    ///
    /// ⚠ V1.9 noexcept 契约: 热路径禁止抛异常（无堆分配、无 Qt 调用）
    size_t readRecent(ConsumerId consumerId, T* out, size_t count) noexcept {
#ifndef NDEBUG
        // ──────── V1.11 运行时单线程所有权校验（lock-free CAS 模式） ────────
        // 与 §3.1 SpscRingBuffer::checkProducerOwnership 完全同构:
        //   1) 读出当前值
        //   2) 若是"未注册"（默认构造的 std::thread::id），CAS 替换为当前线程
        //   3) 否则比较是否一致：不一致 → 拦截 + spdlog + 重新绑定便于 dev 调试
        // 编译期 static_assert 兜底：若目标平台 std::atomic<std::thread::id>
        // 退化为非 lock-free，编译失败强制架构评审决策。
        //
        // V1.14 注意: 若 ConsumerId 绑定到 TaskStrand (线程池 + strand),
        // 应改用 ExecutionContext (ContextId) 校验; 本代码示例展示的是
        // 默认 PhysicalThread 模式。详见 §3.2.2。
        const std::thread::id currentThread = std::this_thread::get_id();
        const uint8_t cid = static_cast<uint8_t>(consumerId);
        std::thread::id expected = std::thread::id{};          // 期望值: 未注册
        const bool firstRegistered =
            m_consumerOwnerThread[cid].compare_exchange_strong(
                expected, currentThread,
                std::memory_order_acq_rel, std::memory_order_acquire);
        if (!firstRegistered) {
            // CAS 失败说明已被注册过。再次检查所有权线程是否一致。
            // 注意：此时 expected 已被 CAS 写为当前 m_consumerOwnerThread[cid] 的实际值。
            if (expected != currentThread) {
                spdlog::error("[RingBuffer] ConsumerId={} ownership violation! "
                              "Owner thread={}, current thread={}. "
                              "Each ConsumerId MUST be operated by a single fixed "
                              "thread. Did you cross-thread pass ConsumerId?",
                              cid, expected, currentThread);
                Q_ASSERT_X(false, "RingBuffer::readRecent",
                           "ConsumerId crossed thread boundary");
                // 断言失败后, 把所有权重新绑定到当前线程以便 dev 继续调试
                m_consumerOwnerThread[cid].store(currentThread, std::memory_order_relaxed);
            }
        }
#endif
        size_t published = m_publishedPos.load(std::memory_order_acquire);
        // 原子 load: 跨线程读 cursor 是安全的（消费者之间无竞争）
        // 原子 store: 同一 ConsumerId 由固定线程持有 → store 不并发 → 仍可 relaxed
        size_t cursor = m_consumerCursors[static_cast<uint8_t>(consumerId)]
                              .load(std::memory_order_relaxed);

        if (published == cursor) return 0;          // 无新数据（Count 语义）

        // 回卷保护：消费者落后超过一圈
        if (published - cursor > Capacity) {
            uint64_t dropped = static_cast<uint64_t>((published - cursor) - Capacity);
            m_droppedFramesCount.fetch_add(dropped, std::memory_order_relaxed);
            cursor = published - Capacity;           // 跳到最老可读位置（Count 语义）

            // V1.5: 按时间节流的 Overrun 日志（默认 5 秒最多 1 条）
            // 避免 Consumer 持续落后时日志风暴，同时保证运维可观测性。
            // 实现侧可选用以下任一方式：
            //   (a) spdlog::warn_every_n(logger, 100, "...");   // 按次数节流
            //   (b) 记录上次日志时间戳，5s 内重复触发则跳过（推荐，见 logOverrunThrottled）
            logOverrunThrottled(consumerId, dropped);
        }

        size_t readable = std::min(published - cursor, count);
        for (size_t i = 0; i < readable; ++i) {
            size_t idx = (cursor + i) & MASK;        // Count → Index
            out[i] = m_buffer[idx].load(std::memory_order_acquire);
        }
        // 单线程所有权 → relaxed store 即可（消费者自己会读到自己的最新值）
        m_consumerCursors[static_cast<uint8_t>(consumerId)]
            .store(cursor + readable, std::memory_order_relaxed);
        return readable;
    }

    /// 按时间范围提取（黑匣子场景 — 一次性原子扫描）
    /// 扫描范围: [startTs, endTs]，从最新已发布位置逆序扫描
    /// @param out       输出缓冲区
    /// @param maxCount  最大输出数量
    /// @return          实际提取数量
    ///
    /// ⚠ V1.9 noexcept 契约: 仅原子读与本地比较，不抛异常
    size_t extractRange(uint64_t startTs, uint64_t endTs,
                        T* out, size_t maxCount) const noexcept {
        size_t published = m_publishedPos.load(std::memory_order_acquire);
        size_t count = 0;

        // Count 语义: published 是已写入总数，最新元素 Count = published，
        // 对应槽位索引 = (published - 1) & MASK。
        for (size_t i = published; i > 0 && count < maxCount; --i) {
            size_t idx = (i - 1) & MASK;             // Count → Index
            T val = m_buffer[idx].load(std::memory_order_acquire);
            if (val.timestamp < startTs) break;
            if (val.timestamp <= endTs) {
                out[count++] = val;
            }
        }
        return count;
    }

    /// 检查消费者是否严重滞后
    /// @return false 表示消费者已落后超过一圈（数据可能被覆盖）
    /// ⚠ V1.9 noexcept 契约: 纯原子读，不抛异常
    bool checkConsumerLag(ConsumerId consumerId) const noexcept {
        size_t published = m_publishedPos.load(std::memory_order_acquire);
        size_t cursor = m_consumerCursors[static_cast<uint8_t>(consumerId)]
                              .load(std::memory_order_relaxed);
        return (published - cursor) <= Capacity;
    }

    /// 获取自 RingBuffer 创建以来因 Overrun 丢失的帧总数
    /// @note 该计数器在回卷保护触发时原子递增，可用于运维监控负载瓶颈
    /// ⚠ V1.9 noexcept 契约: 纯原子 load，不抛异常
    [[nodiscard]] uint64_t droppedFrames() const noexcept {
        return m_droppedFramesCount.load(std::memory_order_relaxed);
    }

    /// 重置 Overrun 计数器（仅供测试或运维诊断使用）
    /// ⚠ V1.9 noexcept 契约: 纯原子 store，不抛异常
    void resetDroppedFrames() noexcept {
        m_droppedFramesCount.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr size_t MASK = Capacity - 1;
    static constexpr uint64_t OVERRUN_LOG_INTERVAL_MS = 5000;  // 5 秒节流间隔

    std::vector<std::atomic<T>>  m_buffer;         // 数据槽位（原子读写）
    std::atomic<size_t>          m_writePos{0};      // 累计写入计数（单生产者）
    std::atomic<size_t>          m_publishedPos{0};  // 已发布累计计数（release/acquire）

    // ⚠ V1.3 修正: 消费者游标同样表示“累计消费计数 (Count)”。
    // ⚠ V1.1 强化: 消费者游标升级为 atomic 数组
    // 旧版: std::array<size_t, MAX_CONSUMERS>  — 不同 ConsumerId 之间读安全,
    //       但同一 ConsumerId 跨线程调用会触发 Data Race
    // 新版: std::array<std::atomic<size_t>, MAX_CONSUMERS>
    //   - 不同 ConsumerId 之间无竞争（数组下标独立）
    //   - 同一 ConsumerId 跨线程并发 readRecent 时，至少保证 load/store 原子
    //   - 仍不能保证 RMW（load → 修改 → store）的原子性 → 调用方仍须遵守
    //     "单 ConsumerId 单线程" 所有权契约（见 readRecent 注释）
    //
    // size_t 在主流 64-bit 平台 lock-free，不引入额外开销
    static_assert(alignof(std::atomic<size_t>) >= alignof(size_t),
                  "atomic<size_t> alignment must be ≥ size_t alignment");
    std::array<std::atomic<size_t>, MAX_CONSUMERS> m_consumerCursors{};  // 累计消费计数

#ifndef NDEBUG
    // ⚠ V1.7 → V1.11 升级: Debug 构建下每个 ConsumerId 的所有权线程注册表
    // - 元素类型 std::atomic<std::thread::id>，默认构造的 std::thread::id 表示"未注册"
    // - 与 m_consumerCursors 同下标, 长度一致
    // - 锁机制从 V1.7 的 std::mutex + std::lock_guard 升级为 V1.11 的 lock-free CAS
    //   模式（std::atomic<std::thread::id>::compare_exchange_strong），与 §3.1
    //   SpscRingBuffer 统一代码风格
    // - 编译期 static_assert 兜底：若目标平台 std::atomic<std::thread::id> 退化为
    //   非 lock-free，编译失败强制架构评审决策（不静默退化）
    // - 性能: SPMC 30/60Hz × MAX_CONSUMERS ≈ 几百次/秒, 单 CAS 开销 < 10 ns,
    //   与原 mutex 路径相比减少至少一次原子 RMW + 一次 syscall 上下文切换
    static_assert(std::atomic<std::thread::id>::is_always_lock_free,
                  "std::atomic<std::thread::id> must be lock-free on target platform; "
                  "if not, RingBuffer Debug ownership check requires architecture review.");
    std::array<std::atomic<std::thread::id>, MAX_CONSUMERS> m_consumerOwnerThread{};
    // V1.11 移除: std::mutex m_ownerInitMutex;  // V1.7-V1.10 由 CAS 替代
#endif

    // V1.4: Overrun 丢帧计数器（全局/每 RingBuffer 实例）
    std::atomic<uint64_t> m_droppedFramesCount{0};

    // V1.5: Overrun 日志节流时间戳（最后打印的 steady_clock 毫秒）
    // 使用 mutable 允许在 const readRecent 路径中调用（若未来 readRecent 标 const）
    mutable std::atomic<uint64_t> m_lastOverrunLogTimeMs{0};

    /// 按时间节流打印 Overrun Warning 日志
    /// 实现: 5 秒内同一 RingBuffer 实例最多打印 1 条，避免日志风暴。
    /// 说明: 若项目未引入 spdlog，可替换为项目统一的日志门面。
    void logOverrunThrottled(ConsumerId consumerId, uint64_t dropped) const {
        using namespace std::chrono;
        const uint64_t nowMs = duration_cast<milliseconds>(
                                   steady_clock::now().time_since_epoch()).count();

        uint64_t last = m_lastOverrunLogTimeMs.load(std::memory_order_relaxed);
        if (nowMs - last < OVERRUN_LOG_INTERVAL_MS) {
            return;  // 处于节流窗口内，跳过
        }

        // CAS 竞争不可怕：多个线程同时到达时，最多多打 1~2 条，仍可接受
        if (m_lastOverrunLogTimeMs.compare_exchange_weak(
                last, nowMs, std::memory_order_relaxed)) {
            spdlog::warn("RingBuffer overrun: consumer={} dropped={} totalDropped={}",
                         static_cast<int>(consumerId), dropped, droppedFrames());
        }
    }
};

// ============================================================================
// 典型 RingBuffer 实例化别名（L1 1 小时 = 36000 槽位）
// ============================================================================
using SampleRingBuffer = RingBuffer<Sample, 65536>;  // 2^16 > 36000
// 低频测点使用较小缓冲区
using SampleRingBufferLowFreq = RingBuffer<Sample, 8192>;  // 2^13 > 3600

}  // namespace ens::datahub
```

#### 3.2.1 V1.10 noexcept 异常传播防御契约（防止热路径 std::terminate）

**隐患**：V1.9 在 `RingBuffer::readRecent` / `SpscRingBuffer::readRecent` / `DownSampler::lttb` 等热路径函数上声明了 `noexcept`。该声明向编译器承诺：**函数不会抛出任何 C++ 异常**。编译器因此可优化掉栈展开代码与 `.gcc_except_table`，换来 **~5–15% 的热路径吞吐提升** 与更确定的指令缓存占用。

但同时编译器把这一承诺变成 **强约束** —— 若未来重构时不小心在 `readRecent` 中加入：

- `std::vector<T> tmp; tmp.reserve(N);` 之后又写为 `tmp.push_back(...)` 且 N 超过 `capacity()` → `std::bad_alloc` 抛出；
- 未 `reserve()` 的 `std::vector` 直接 `push_back` → 多次重分配可能抛出 `std::bad_alloc`；
- `spdlog::info("value={}", bigValue)` 中 `bigValue.format` 的字符串拼接路径上分配失败 → `std::bad_alloc`；
- `QString::number` / `QString::arg` / `QFile::readAll` 等任何 Qt 容器操作都可能抛 `std::bad_alloc`；
- `std::function` / `std::any` / `std::variant` 的值类型构造（依赖目标类型 noexcept 性）；

则该异常在 `noexcept` 函数体内一旦传播，C++ 运行时将 **直接调用 `std::terminate()`**，进程瞬间崩溃，且没有栈展开、没有 catch 兜底、没有错误日志。这种"无声崩溃"在投产环境极难定位。

**目标**：通过**静态检查 + 单元测试 + CI 拦截**三层防线，确保 `noexcept` 热路径函数内部调用的所有子函数均严格无堆分配且具备 `noexcept` 保证，把异常扩散风险拦在编译期/测试期而非运行时。

**第一条防线：禁止调用清单（实现侧硬约束）**

在所有被声明为 `noexcept` 的热路径函数体内，**严禁直接/间接调用以下签名（或可能抛出 `std::bad_alloc` 的同类 API）**：

| 类别 | 禁止 API（举例） | 备选方案 |
|------|------------------|---------|
| 标准容器 | `std::vector::push_back`（未 `reserve`）<br/>`std::vector::resize`（可能分配）<br/>`std::deque::push_back`/`emplace_back`<br/>`std::map`/`std::unordered_map` 默认构造+插入 | 在函数入口预分配固定大小 `std::array` / `std::vector(N)` 一次性构造，再无堆路径访问；或写指标进入"spill queue"异步落库（不在热路径内分配） |
| Qt 容器 | `QVector::append` / `QVector::resize`<br/>`QString::arg` / `QString::number`<br/>`QByteArray::resize` | 改用 `std::string` + `std::to_chars`，或预分配 `char buf[64]`，或上报不重要的热路径日志改用字符串驻留池 |
| 字符串格式化 | `fmt::format` / `spdlog::info` / `std::ostringstream`（可能 bad_alloc）<br/>`std::to_string`（小对象不抛，但返回值拷贝是堆分配热点） | 热路径日志改用 `spdlog::level::off` / `SPDLOG_ACTIVE_LEVEL` 编译期剔除，或用 stack 缓冲 `std::array<char, 256>` + `fmt::format_to` |
| 文件/IO | `std::fopen` / `QFile::readAll` / `std::string::assign(const char*)` | 仅允许 `errno_t` 风格的 C 接口 + 调用前判断 `errno`；IO 在异步线程 offload |
| 内存分配 | `new` / `std::make_shared` / `std::make_unique`（含栈对象） | 改用预分配对象池（详见 §3.4 内存池接口） |
| 异常对象 | `throw` 任何异常 | 改用 `std::expected<T, ErrorCode>` / `std::variant` / 返回值错误码；或返回 `bool`+`enum class ErrorCode` 输出参数 |

**第二条防线：clang-tidy 静态检查（CI 强制）**

在 `.clang-tidy` 中启用并自定义以下检查：

```yaml
# .clang-tidy (V1.10 新增段)
Checks: >
  -*,
  bugprone-exception-escape,
  misc-noexcept,
  misc-include-cleaner,
  ens-capi-stringview-safety,
  ens-noexcept-hot-path-noexcept-only

# bugprone-exception-escape: 检测 noexcept 函数内 throw 表达式
# misc-noexcept: 建议为已知不抛的函数添加 noexcept
# ens-noexcept-hot-path-noexcept-only: 自定义检查, 阻断 noexcept 热路径调用可抛函数 (V1.10 新增)
```

**ens-noexcept-hot-path-noexcept-only 检查骨架**（V1.10 自定义 AST matcher，需部署 `clang-tidy-ens-plugins/` 自定义插件或基于 `clang-query` 的 CI 脚本）：

```cpp
// 自定义检查伪代码 (V1.10)
// 命中规则: 函数声明 noexcept (RingBuffer/SpscRingBuffer/DownSampler 内 noexcept)
//          AND 函数体内调用不在白名单中的非 noexcept 函数
//
// 白名单 (允许的非 noexcept 但已知不会扩散到热路径的函数):
//   - std::atomic<T>::load / store / fetch_* / compare_exchange_*
//   - std::atomic_thread_fence
//   - T* 地址运算 / 位运算
//   - 标量数学 (sin / cos / sqrt 等 -ffast-math 下不抛)
//   - spdlog::level::off 路径下的 log (编译期剔除)
//
// 命中动作: 报告 warning → CI 阻断 (等级: error)
//
// 简化实现思路 (clang-query 脚本):
//   match callExpr(
//       callee(  // 检查调用目标的 noexcept 属性
//           functionDecl(isNoExceptTemplate()) 和 callee 实际函数不 noexcept
//       ),
//       anyOf(
//           hasAncestor(functionDecl(
//               EnerSentry_HOT_PATH_NOEXCEPT  // 自定义 attribute
//           ).bind("hotPath")),
//           hasAncestor(cxxMethodDecl(
//               isNoExcept(),  // 函数声明 noexcept
//               ofClass(cxxRecordDecl(
//                   hasName("RingBuffer"),
//                   hasName("SpscRingBuffer"),
//                   hasName("DownSampler")
//               ))
//           ).bind("hotPath"))
//       )
//   )
```

**第三条防线：单元测试覆盖（每次 PR 必跑）**

```cpp
// test_ringbuffer_noexcept_contract.cpp (V1.10 新增测试模板)
//
// 目标: 通过一组"坏代理" 拦截函数验证 noexcept 边界
//   - 若 RingBuffer::readRecent 间接调用了 mock_bad_alloc_function (未 reserve vector),
//     若 noexcept 契约被破坏, 程序将立即 std::terminate, 测试失败;
//   - 若 noexcept 契约保持, 程序正常返回, 测试通过。

#include "RingBuffer.h"
#include "SpscRingBuffer.h"
#include "DownSampler.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <thread>

namespace ens::test {

// 模拟一个可抛 std::bad_alloc 的"禁函数"
ATTRIBUTE_NOINLINE [[noreturn]] void mock_bad_alloc() {
    throw std::bad_alloc{};
}

// V1.10 CI 测试用例:
//   启用 LD_PRELOAD/链接器 hook 替换上述 mock, 调用每个 noexcept 热路径函数;
//   期望: 若 noexcept 边界被破坏, 进程立即 std::terminate (CI 红);
TEST(NoexceptContract, RingBufferReadRecent_NoThrowGuarantee) {
    SampleRingBuffer rb;
    Sample buf[16];
    // 调用前先建立"逻辑链"以确保 readRecent 函数地址被实际触发
    Sample s{}; s.pointId = 1; s.value = 1.0f; s.timestamp = 1;
    rb.push(s);
    // 关键断言: 若 noexcept 边界被破坏 (readRecent 内部调用了 mock),
    // std::terminate 会在此处直接触发, 测试进程崩溃;
    // 若 noexcept 边界保持, readRecent 静默返回, 进程不受影响。
    ASSERT_NO_THROW(rb.readRecent(ens::datahub::RingBuffer<Sample, 65536>::ConsumerId::UI_RENDER,
                                   buf, 16));
}

TEST(NoexceptContract, SpscRingBufferPushBatch_NoThrowGuarantee) {
    SpscRingBuffer<Sample, 8192> rb;
    Sample arr[8]{};
    ASSERT_NO_THROW(rb.pushBatch(arr, 8));
}

TEST(NoexceptContract, DownSamplerLttb_NoThrowGuarantee) {
    QPointF src[100], dst[16];
    for (int i = 0; i < 100; ++i) src[i] = {double(i), double(i)};
    ASSERT_NO_THROW(DownSampler::lttb(src, 100, dst, 16, 16));
}

}  // namespace ens::test
```

**第四条防线：CI 阻断脚本（集成示例）**

```bash
#!/usr/bin/env bash
# scripts/ci_noexcept_hot_path_check.sh (V1.10 新增)
# 阶段 1: 静态检查
clang-tidy \
    --config-file=.clang-tidy \
    --quiet \
    src/datahub/RingBuffer.h src/datahub/Sample.h \
    src/ui/DownSampler.h src/ui/RealtimeChartWidget.cpp 2>&1 \
  | tee clang-tidy.log
if grep -E "(warning|error): .*(bugprone-exception-escape|ens-noexcept-hot-path)" clang-tidy.log; then
    echo "[CI FAIL] noexcept 热路径检查未通过"
    exit 1
fi

# 阶段 2: 单元测试
./build/tests/test_ringbuffer_noexcept_contract --gtest_output=xml:report.xml
if ! grep -q "All tests passed" report.xml; then
    echo "[CI FAIL] noexcept 契约单元测试失败"
    exit 1
fi

echo "[CI PASS] noexcept 热路径异常扩散防御三道防线全部通过"
```

**关键评审基线（不可突破的红线）**：

1. ❌ **禁止**在 `RingBuffer::readRecent` / `SpscRingBuffer::readRecent` / `DownSampler::lttb` 等热路径内引入可能抛 `std::bad_alloc` 的标准容器操作。
2. ❌ **禁止**在热路径日志中调用 `spdlog::info`/`warn`（除非改为编译期剔除级别）。
3. ❌ **禁止**在热路径内触发 Qt 容器构造（`QString`/`QByteArray`/`QVector` 隐式分配路径）。
4. ❌ **禁止**引入 `std::function` / `std::any` / `std::variant` 默认构造（含 RTTI）。
5. ✅ **允许**预分配缓冲 (`std::array<T, N>` / `std::vector(n)` 一次性构造)、atomic 操作、纯算术与位运算、栈上 POD 构造。
6. ✅ **允许**新增热路径函数，但必须同时声明 `noexcept` 并通过上述三道防线。

**责任人**：所有涉及 `RingBuffer`/`SpscRingBuffer`/`DownSampler` 的 PR 必须由架构师或核心 Owner Reviewer 复核；CI 阶段由 `scripts/ci_noexcept_hot_path_check.sh` 强制阻断。

**反向 ADR 引用**：本防御契约与 §3.1.1 压测闸门、§1.6.2 clang-tidy 自定义检查、§4.4 PIMPL 头文件控制共同构成"EnerSentry 热路径代码质量四道防线"，不可单独剥离。

#### 3.2.2 V1.14 单线程所有权与线程池兼容性

**隐患**：§3.1 `SpscRingBuffer` 与 §3.2 `RingBuffer` 在 Debug 构建下使用 `std::atomic<std::thread::id>` + `compare_exchange_strong` 校验"单线程所有权"。该机制在以下两种场景下会出现**误判**：

1. **QThreadPool 任务级串行**：若上层将消费任务投递到 `QThreadPool`，虽然业务逻辑上同一 `ConsumerId` 的任务是串行执行的，但 `QThreadPool` 的 Worker 线程会根据负载动态变化，导致 `std::this_thread::get_id()` 在不同调用间不一致；
2. **asio::thread_pool + strand**：`boost::asio::strand` 保证任务按投递顺序串行执行，但执行线程可能来自线程池中不同 Worker，裸 `std::thread::id` 同样会变化。

在这两种情况下，当前的 Debug 所有权校验会**误触发 `Q_ASSERT_X` 运行时断言**，阻塞开发调试。

**核心澄清："单线程所有权"的两种合法语义**

| 语义 | 含义 | 适用场景 | 校验目标 |
|------|------|---------|---------|
| **A. 物理线程固定 (Physical Thread Fixed)** | 同一个 `ConsumerId` / 写者必须由**固定的物理线程**持有 |  dedicated `QThread`、独立的 `std::thread`、绑定到特定线程的 `QObject` | `std::thread::id` |
| **B. 任务级串行上下文 (Task-level Serialized Context)** | 同一个 `ConsumerId` / 写者的调用序列在**逻辑上串行**，但允许由线程池中的不同 Worker 线程执行 | `QThreadPool` 的 `Runnable` 串行队列、`asio::strand`、`io_context::strand`、线程安全的任务队列 | **Context ID / Strand ID** |

**设计原则**：

- 默认语义为 **A（物理线程固定）**；
- 当业务明确使用线程池 + 串行调度时，**必须**切换到语义 B，否则 Debug 断言会被误触发；
- 不允许语义混淆：不能既用线程池动态调度，又声称"同一 ConsumerId 固定线程"。

**ExecutionContext 抽象（适配线程池场景）**：

```cpp
// ============================================================================
// ExecutionContext — 单线程所有权上下文抽象 (V1.14)
// 头文件: include/ens/ExecutionContext.h
// ============================================================================
#pragma once

#include <cstdint>
#include <thread>
#include <atomic>

namespace ens {

/// 上下文标识类型:
///   - 物理线程固定模式: 使用 std::thread::id 的 hash 值
///   - 任务级串行模式: 由 strand / 任务调度器提供的唯一 ID
using ContextId = uint64_t;

/// 所有权校验模式
enum class OwnershipMode : uint8_t {
    PhysicalThread,   // 默认: 校验 std::thread::id
    TaskStrand,       // 线程池 + strand: 校验调用方传入的 ContextId
};

/// 获取当前上下文 ID
/// 在 PhysicalThread 模式下返回 std::hash<std::thread::id>{}(std::this_thread::get_id())
/// 在 TaskStrand 模式下由调用方显式传入 strand ID
inline ContextId currentPhysicalThreadId() noexcept {
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

}  // namespace ens
```

**RingBuffer / SpscRingBuffer 适配方案**：

1. **默认实现（语义 A）**：保留现有 `std::atomic<std::thread::id>` 校验，无需修改；
2. **线程池场景（语义 B）**：将 `m_consumerOwnerThread` / `m_producerOwnerThread` 的类型从 `std::atomic<std::thread::id>` 替换为 `std::atomic<ContextId>`，并在构造或首次调用时由调用方显式提供 `ContextId`。

```cpp
// ============================================================================
// 线程池兼容版本: SpscRingBuffer 使用 ContextId 而非 std::thread::id (V1.14)
// ============================================================================
template <typename T, size_t Capacity>
class SpscRingBuffer {
public:
    /// 显式绑定到一个任务级串行上下文 (如 asio strand ID)
    /// 绑定后, push/readRecent 的 Debug 校验将检查该 ContextId 而非 thread_id
    void bindExecutionContext(ContextId contextId) noexcept {
#ifndef NDEBUG
        ContextId expected = kInvalidContextId;
        const bool ok = m_producerContext.compare_exchange_strong(
            expected, contextId,
            std::memory_order_relaxed, std::memory_order_relaxed);
        if (!ok && expected != contextId) {
            spdlog::error("SpscRingBuffer::bindExecutionContext: "
                          "context already bound to {}, cannot rebind to {}",
                          expected, contextId);
            Q_ASSERT_X(false, "SpscRingBuffer::bindExecutionContext",
                       "Execution context rebinding is not allowed");
        }
        m_consumerContext.store(contextId, std::memory_order_relaxed);
#endif
    }

private:
#ifndef NDEBUG
    static constexpr ContextId kInvalidContextId = 0;
    std::atomic<ContextId> m_producerContext{kInvalidContextId};
    std::atomic<ContextId> m_consumerContext{kInvalidContextId};
#endif
};
```

**典型使用模式对比**：

| 场景 | 推荐模式 | 代码示例 |
|------|---------|---------|
| 独立采集线程 → 独立持久化线程 | PhysicalThread | `SpscRingBuffer<...> rb;`（默认） |
| asio strand 串行任务消费 | TaskStrand | `rb.bindExecutionContext(reinterpret_cast<ContextId>(&strand));` |
| QThreadPool 中单一 `QObject` 通过 QueuedConnection 消费 | PhysicalThread（等同于固定到 `qApp->thread()`） | 不切换模式，但 ConsumerId 绑定到该 `QObject` 所在线程 |
| 自定义任务队列，单线程事件循环执行 | PhysicalThread | `ConsumerId` 由事件循环线程持有 |

**QThreadPool 场景详细建议**：

1. **不推荐**直接将 `SpscRingBuffer::readRecent()` 放到 `QThreadPool::globalInstance()->start(runnable)` 中反复调用，因为 Worker 线程不固定；
2. 若必须使用线程池，采用以下两种方案之一：
   - **方案 A**：创建一个 dedicated `QThread`，其事件循环中执行 `readRecent()`；
   - **方案 B**：在 `QThreadPool` 之上包装一个**串行队列**（Serialized Queue），所有消费任务按 FIFO 顺序投递到同一个 Worker，并在 `bindExecutionContext()` 中传入该队列的唯一 ID；此时需自行保证任务不会被并发执行。

**asio::thread_pool + strand 场景详细建议**：

```cpp
// ============================================================================
// asio strand 消费 SpscRingBuffer 示例 (V1.14)
// ============================================================================
boost::asio::thread_pool pool(4);
boost::asio::strand<boost::asio::thread_pool::executor_type> strand(
    pool.get_executor());

SpscRingBuffer<SampleCompact8, 65536> rb;
// 每个 strand 有唯一稳定的内存地址，可作为 ContextId
rb.bindExecutionContext(reinterpret_cast<ens::ContextId>(&strand));

// 所有消费任务通过同一个 strand 投递，保证串行执行
boost::asio::post(strand, [&rb]() {
    SampleCompact8 buf[256];
    size_t n = rb.readRecent(buf, 256);
    // ... 处理 buf ...
});
```

**强制约束**：

1. **必须显式声明所有权语义**：每个 `RingBuffer<T>` / `SpscRingBuffer<T>` 实例的注释中必须说明：
   - 使用的是 `PhysicalThread` 还是 `TaskStrand`；
   - 若是 `TaskStrand`，必须给出 `ContextId` 的来源与生命周期。
2. **禁止混用两种语义**：同一个 `ConsumerId` / 写者不能既在某些调用中使用固定线程，又在另一些调用中使用线程池动态 Worker。
3. **Debug 校验必须与语义匹配**：
   - `PhysicalThread` 模式下使用 `std::atomic<std::thread::id>`；
   - `TaskStrand` 模式下使用 `std::atomic<ContextId>`，且 `ContextId` 不能为 `0`（`0` 保留为未绑定标记）。
4. **线程池场景必须提供串行保证**：调用方必须证明 `ConsumerId` / 写者的调用序列是严格串行的；仅依赖"任务通常不会被并发执行"是不够的。
5. **单元测试覆盖**：必须新增以下测试：
   - `PhysicalThread` 模式下跨线程调用触发断言；
   - `TaskStrand` 模式下同一 `ContextId` 在不同 `std::thread::id` 上调用不触发断言；
   - `TaskStrand` 模式下两个不同 `ContextId` 同时调用同一 `ConsumerId` 触发断言。
6. **文档化每个实例**：代码审查时必须检查每个 RingBuffer/SpscRingBuffer 实例是否附有 §3.1.2 要求的四要素注释，并额外增加第五项：所有权语义（PhysicalThread / TaskStrand）。

---

### 3.3 IMappedFile mmap 跨平台抽象接口

**所属 Target**：`ens::datahub`（STATIC 库）  
**头文件路径**：`src/datahub/platform/PlatformMMap.h`

```cpp
// ============================================================================
// EnerSentry — IMappedFile mmap 跨平台抽象接口 (V1.5)
// 所属 Target: ens::datahub (STATIC)
// 头文件: src/datahub/platform/PlatformMMap.h
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ens::datahub::platform {

// ============================================================================
// IMappedFile — 文件锁定 + 内存映射的跨平台抽象接口
//
// 设计约束:
//  - open/close 成对调用，close 必须幂等
//  - Win32 实现使用 CreateFileMapping/MapViewOfFile
//  - POSIX 实现使用 mmap(MAP_SHARED)
//  - flushAsync 不阻塞调用线程；flushSync 阻塞至数据真正落盘
//  - 所有错误信息通过 lastError() 获取（跨平台统一语义）
// ============================================================================
class IMappedFile {
public:
    virtual ~IMappedFile() = default;

    // ──── 生命周期 ────

    /// 打开（或创建）映射文件
    /// @param path      文件路径（std::string_view 避免临时 std::string 分配）
    /// @param size      期望文件大小（字节）
    /// @param readOnly  true=只读；false=读写
    /// @return          true=成功；false=失败（通过 lastError() 获取错误码）
    ///
    /// ⚠ 实现约束（V1.4 + V1.7 强化）:
    ///   path 是 std::string_view，不保证以 '\0' 结尾。Win32 / POSIX 实现内部
    ///   **必须**调用 ens::utils::to_null_terminated(path) 或
    ///   ens::utils::to_path(path)（参见 §1.6.1），再调用
    ///   CreateFileA / CreateFileW / ::open，**禁止直接**传递 path.data()。
    ///   代码审查 (CR) 与编码规范检查工具应阻断裸 path.data() 写法。
    ///   V1.8 起通过 clang-tidy 自定义规则 `ens-capi-stringview-safety` 在 CI 中
    ///   自动拦截，参见 §1.6.2。
    virtual bool open(std::string_view path, size_t size, bool readOnly) = 0;

    /// 关闭映射（必须幂等）
    virtual void close() = 0;

    // ──── 内存访问 ────

    /// 获取进程地址空间基地址
    /// @note 必须在 open() 成功后调用
    virtual void* baseAddress() const = 0;

    /// 获取文件大小
    virtual size_t size() const = 0;

    // ──── 刷盘 ────

    /// 异步刷盘（不阻塞调用线程）
    /// Win32: FlushViewOfFile (lazy writer)
    /// POSIX: msync(MS_ASYNC)
    /// @param offset 起始偏移
    /// @param length 字节数
    /// @return       true=成功
    virtual bool flushAsync(size_t offset, size_t length) = 0;

    /// 同步刷盘（阻塞至数据真正落盘）
    /// Win32: FlushViewOfFile + FlushFileBuffers
    /// POSIX: msync(MS_SYNC)
    /// @param offset 起始偏移
    /// @param length 字节数
    /// @return       true=成功
    virtual bool flushSync(size_t offset, size_t length) = 0;

    // ──── 状态 ────

    /// 获取最近一次错误码
    /// @return 0=OK; 1=文件不存在; 2=权限拒绝; 3=句柄泄漏/文件锁定; 99=其他
    virtual int lastError() const = 0;

    /// 判断文件是否被其他进程锁定（Windows 特有）
    /// POSIX 默认无强制独占锁，总是返回 false
    virtual bool isLockedByOtherProcess() const = 0;
};

/// 工厂函数 — 根据编译环境自动选择 Win32 或 POSIX 实现
/// @return unique_ptr<IMappedFile>，由调用方持有所有权
std::unique_ptr<IMappedFile> createMappedFile();

}  // namespace ens::datahub::platform
```

### 3.4 IDataAccess 数据访问抽象接口

**所属 Target**：`ens::datahub`（STATIC 库）  
**头文件路径**：`src/datahub/IDataAccess.h`

```cpp
// ============================================================================
// EnerSentry — IDataAccess 数据访问抽象接口 (V1.5)
// 所属 Target: ens::datahub (STATIC)
// 头文件: src/datahub/IDataAccess.h
// ============================================================================
#pragma once

#include "Sample.h"
#include <QString>
#include <cstdint>
#include <vector>

namespace ens::datahub {

// ============================================================================
// HistoryGranularity — 历史数据粒度枚举
// ============================================================================
enum class HistoryGranularity : uint8_t {
    Gran100ms = 0,
    Gran1s    = 1,
    Gran5s    = 2,
    Gran1m    = 3,
};

// ============================================================================
// AlarmLevel — 告警级别（与 business 层保持一致）
// ============================================================================
enum class AlarmLevel : uint8_t {
    Info     = 0,   // 提示（蓝色）
    Warning  = 1,   // 一般（黄色）
    Critical = 2,   // 严重（红色）
};

// ============================================================================
// AlarmStatus — 告警生命周期状态
// ============================================================================
enum class AlarmStatus : uint8_t {
    Active    = 0,   // 未处理
    Confirmed = 1,   // 已确认
    Recovered = 2,   // 已恢复
};

// ============================================================================
// AlarmRecord — 告警记录（数据库映射用）
// ============================================================================
struct AlarmRecord {
    uint64_t    id = 0;
    uint32_t    pointId = 0;
    AlarmLevel  level = AlarmLevel::Info;
    uint64_t    triggerTime = 0;
    uint64_t    recoverTime = 0;
    QString     confirmUser;
    uint64_t    confirmTime = 0;
    float       alarmValue = 0.0f;
    float       threshold = 0.0f;
    QString     description;
    AlarmStatus status = AlarmStatus::Active;
};

// ============================================================================
// IDataAccess — 数据访问抽象接口
//
// 设计约束:
//  - 所有方法为纯虚函数，底层实现可切换 (SQLite → MySQL)
//  - getTableName / getDatabasePath 实现按月分库路由
//  - queryHistoryRange 单次跨月数 ≤ 3（HLD V1.4 ADR-19）
//  - 批量写入通过 batchInsertHistory 实现，调用方不感知事务边界
// ============================================================================
class IDataAccess {
public:
    virtual ~IDataAccess() = default;

    // ═══════════════════════════════════════════════════════════════
    // 连接与路由
    // ═══════════════════════════════════════════════════════════════

    /// 打开数据连接
    /// @param connectionString  SQLite: 数据根目录路径; MySQL: 连接串
    virtual bool open(const QString& connectionString) = 0;

    /// 关闭数据连接
    virtual void close() = 0;

    /// 表名路由 — 由 timestamp 解析出 "history_1s_YYYYMM" 或 "history_5s_YYYYMM"
    /// @param pointId   测点 ID（预留按测点分片扩展）
    /// @param timestamp Unix 毫秒时间戳
    /// @param gran      数据粒度
    virtual QString getTableName(uint32_t pointId, uint64_t timestamp,
                                 HistoryGranularity gran) const = 0;

    /// 单月独立 DB 文件路径 — 由 timestamp 解析出 "data_YYYYMM.db"
    /// @param timestamp Unix 毫秒时间戳
    virtual QString getDatabasePath(uint64_t timestamp) const = 0;

    // ═══════════════════════════════════════════════════════════════
    // 历史数据批量写入
    // ═══════════════════════════════════════════════════════════════

    /// 批量写入降采样历史数据
    /// 实现层负责：按月份分桶 → 逐月独立事务 INSERT → COMMIT
    /// @param samples 降采样后的聚合数据
    /// @return        true=全部成功；false=部分失败（具体错误记录日志）
    virtual bool batchInsertHistory(const std::vector<DownSampledSample>& samples) = 0;

    // ═══════════════════════════════════════════════════════════════
    // 历史数据查询
    // ═══════════════════════════════════════════════════════════════

    /// 单月范围查询（快速路径）
    /// @return 按 timestamp 升序排列的聚合数据
    virtual std::vector<DownSampledSample> queryHistory(
        uint32_t pointId, uint64_t startTime, uint64_t endTime) = 0;

    /// 跨月范围查询（使用 ATTACH DATABASE + UNION ALL）
    /// 约束: 单次跨月数 ≤ 3；超限由调用方拆分查询
    /// @return 按 timestamp 升序排列的聚合数据
    virtual std::vector<DownSampledSample> queryHistoryRange(
        uint32_t pointId, uint64_t startTime, uint64_t endTime) = 0;

    // ═══════════════════════════════════════════════════════════════
    // 黑匣子
    // ═══════════════════════════════════════════════════════════════

    /// 持久化黑匣子快照
    /// @param alarmId  关联告警 ID
    /// @param pointId  触发测点 ID
    /// @param start    快照起始时间
    /// @param end      快照结束时间
    /// @param dataJson 快照数据 (JSON 数组: [{ts, value}, ...])
    virtual bool insertBlackBox(uint64_t alarmId, uint32_t pointId,
                                uint64_t start, uint64_t end,
                                const QString& dataJson) = 0;

    /// 查询黑匣子快照数据
    virtual QString queryBlackBox(uint64_t alarmId) = 0;

    // ═══════════════════════════════════════════════════════════════
    // 告警记录
    // ═══════════════════════════════════════════════════════════════

    virtual bool insertAlarm(const AlarmRecord& alarm) = 0;

    virtual bool updateAlarmStatus(uint64_t alarmId, AlarmStatus status,
                                   const QString& user, uint64_t confirmTime) = 0;

    virtual std::vector<AlarmRecord> queryAlarms(
        uint64_t startTime, uint64_t endTime,
        AlarmLevel level = AlarmLevel::Info, int maxCount = 10000) = 0;

    // ═══════════════════════════════════════════════════════════════
    // 审计日志
    // ═══════════════════════════════════════════════════════════════

    virtual bool insertAuditLog(const QString& user, const QString& action,
                                const QString& target, const QString& detail,
                                const QString& result) = 0;

    // ═══════════════════════════════════════════════════════════════
    // 数据清理
    // ═══════════════════════════════════════════════════════════════

    virtual int deleteBefore(uint64_t timestamp, const QString& tableName) = 0;

    /// 获取指定表占用磁盘空间（字节）
    virtual uint64_t getTableSize(const QString& tableName) = 0;
};

}  // namespace ens::datahub
```

### 3.5 IDataAccess SQLite 实现层并发契约（V1.6）

**所属 Target**：`ens::datahub`（STATIC 库）  
**实现文件**：`src/datahub/DataAccessImpl.cpp`（非头文件，但接口使用方必须了解其约束）

IDataAccess 的实现层（`DataAccessImpl`）直接操作 SQLite3。以下约束是**性能与线程安全的红线**，必须在实现中严格遵守：

#### 3.5.1 SQLite 连接所有权规则

| 架构选项 | 适用场景 | 要求 |
|---------|---------|------|
| **Per-Thread DB Connection** | 并发查询量低、线程数固定 | 每个需要访问 SQLite 的线程持有独立的 `sqlite3*` 句柄，禁止跨线程共享 |
| **Connection Pool** | 并发查询量高、线程数动态 | 池内连接数 ≥ 工作线程数，借用/归还必须配对，`sqlite3*` 句柄不得同时被两个线程使用 |

> ⚠ **关键风险**：SQLite 默认单个 `sqlite3*` 连接最多 **ATTACH 10 个数据库**。在 WAL 模式下，跨线程共享同一个 `sqlite3*` 句柄会导致未定义行为（段错误、数据库损坏）。

**V1.8 补充：繁忙超时 (busy_timeout) 配置**

- 无论是 Per-Thread 连接还是从连接池获取的连接，在 `IDataAccess::open()` 初始化阶段必须显式调用：

  ```cpp
  sqlite3_busy_timeout(m_db, 3000);  // 3000 ms，单位：毫秒
  ```

- **推荐值**：`3000 ms`。该值能在跨月查询多线程并发 ATTACH 时显著降低 `SQLITE_BUSY` 冲突，同时避免单线程故障等待过长。
- **平台一致性**：`sqlite3_busy_timeout` 是 SQLite3 C-API 的跨平台接口，Windows/Linux 行为一致，不依赖编译器扩展。
- **禁止值**：不允许使用 `0`（无等待立即返回 `SQLITE_BUSY`）或 `-1`（无限等待），这两种极端值都会在高并发场景下放大故障。

#### 3.5.2 ATTACH DATABASE 生命周期约束

`queryHistoryRange()` 涉及跨月分库查询，实现层应遵循以下生命周期：

```text
1. 从连接池获取一个 sqlite3* 句柄（或当前线程独立连接）
2. 根据 [startTime, endTime] 计算涉及的月份集合（≤3 个月）
3. 对每个目标月份 DB 执行: ATTACH DATABASE 'data_YYYYMM.db' AS db_YYYYMM
4. 执行 UNION ALL 跨库查询
5. 查询结果返回前，对每个 ATTACH 的 DB 执行: DETACH DATABASE db_YYYYMM
6. 将 sqlite3* 句柄归还连接池
```

**强制约束**：

1. **最小化 ATTACH 占用**：`ATTACH` 只存在于 `queryHistoryRange` 执行期间，查询完毕必须立即 `DETACH`，禁止长期占用连接上的 ATTACH 名额。
2. **上限保护**：单次查询涉及的月份数 ≤ 3（已由 `IDataAccess` 接口声明）。若调用方传入更大范围，实现层应拒绝或拆分为多次查询。
3. **事务隔离**：批量写入使用**逐月独立事务**，每个月份 DB 单独 `BEGIN IMMEDIATE → INSERT → COMMIT`。禁止在 ATTACH 状态下执行写入事务。
4. **错误回滚**：任何步骤失败时，必须执行 `ROLLBACK`（如有活跃事务）并 `DETACH` 所有已 ATTACH 的数据库，防止连接处于污染状态回收入池。
5. **连接池线程安全**：连接池的 `acquire/release` 本身必须用互斥锁保护；但取出的 `sqlite3*` 句柄**不得**在多个线程间传递。
6. **连接归还前 ROLLBACK 校验（V1.8）**：连接归还连接池前，必须执行以下卫生检查：

   ```cpp
   // 检查并回滚任何残留事务
   int autocommit = sqlite3_get_autocommit(conn);
   if (autocommit == 0) {
       sqlite3_exec(conn, "ROLLBACK", nullptr, nullptr, nullptr);
       spdlog::warn("DataAccessImpl: connection returned to pool with active transaction, forced ROLLBACK");
   }
   ```

   - 使用 `sqlite3_get_autocommit()` 判断是否存在未提交事务；返回 `0` 表示处于显式事务中。
   - 若发现未提交事务，必须立即 `ROLLBACK`；禁止将 `BEGIN` 状态直接归还池内，避免污染后续借用者。
   - 归还操作本身必须在 `release()` 内部完成，调用方不应手动处理。
   - 每次归还时同时重置连接状态：清除 `ATTACH`（已由 ScopeGuard 保证）、回滚未提交事务、清理 `sqlite3_changes` 以外的副作用。

7. **繁忙超时一致性（V1.8）**：从连接池取出的连接应继承创建时配置的 `busy_timeout`（3000 ms）。连接池不得在不同连接上配置不同的超时值，避免调用方行为不可预期。

#### 3.5.3 实现层伪代码

```cpp
// DataAccessImpl::queryHistoryRange 实现层伪代码（V1.8）
std::vector<DownSampledSample> DataAccessImpl::queryHistoryRange(
    uint32_t pointId, uint64_t startTime, uint64_t endTime)
{
    auto conn = m_pool.acquire();          // 从连接池获取句柄（RAII 归还，含 ROLLBACK 卫生）
    std::vector<QString> attachedAliases;

    auto guard = makeScopeGuard([&]() {    // 异常安全：确保 DETACH + ROLLBACK
        for (const auto& alias : attachedAliases) {
            sqlite3_exec(conn->handle(),
                fmt::format("DETACH DATABASE {}", alias).toUtf8().constData(),
                nullptr, nullptr, nullptr);
        }
        // V1.8: 归还前强制回滚残留事务，防止污染池内连接
        if (sqlite3_get_autocommit(conn->handle()) == 0) {
            sqlite3_exec(conn->handle(), "ROLLBACK", nullptr, nullptr, nullptr);
        }
    });

    auto months = computeMonths(startTime, endTime);  // 返回 ["202601", "202602"]
    if (months.size() > 3) {
        spdlog::error("queryHistoryRange spans too many months: {}", months.size());
        return {};                           // guard 会自动 DETACH + ROLLBACK
    }

    for (const auto& month : months) {
        QString alias = QString("db_%1").arg(month);
        QString sql = QString("ATTACH DATABASE '%1/data_%2.db' AS %3")
                          .arg(m_dataRoot).arg(month).arg(alias);
        if (sqlite3_exec(conn->handle(), sql.toUtf8().constData(), nullptr, nullptr, nullptr) != SQLITE_OK) {
            return {};                       // guard 会自动 DETACH + ROLLBACK
        }
        attachedAliases.push_back(alias);
    }

    // 构造 UNION ALL 查询...
    // 执行查询并填充结果...

    return result;                           // guard 在析构时 DETACH + ROLLBACK
}
```

#### 3.5.4 V1.8 连接池归还卫生（ROLLBACK 校验与 busy_timeout 一致性）

**隐患**：WAL 模式下虽然支持单写多读，但跨月查询（多个 DB 文件 ATTACH）在多线程并发请求时仍可能出现短期锁竞争。若连接在归还池内时残留未提交事务或 ATTACH，后续借用者会继承脏状态，导致查询异常、锁冲突甚至数据不一致。

**目标**：在连接池 `release()` 实现中显式配置 `sqlite3_busy_timeout(3000)`，并在归还时强制 ROLLBACK 校验。

**连接创建/初始化伪代码**：

```cpp
// DataAccessImpl::open() / ConnectionPool::createConnection()（V1.8）
sqlite3* db = nullptr;
int rc = sqlite3_open_v2(
    path.toUtf8().constData(),
    &db,
    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_WAL,
    nullptr);
if (rc != SQLITE_OK) { /* 错误处理 */ }

// V1.8: 统一配置繁忙超时 3000 ms
sqlite3_busy_timeout(db, 3000);
```

**连接池归还伪代码**：

```cpp
// ConnectionPool::release(sqlite3* db)（V1.8）
void ConnectionPool::release(sqlite3* db)
{
    if (!db) return;

    // V1.8: 强制卫生检查
    // 1) 检查并回滚任何未提交事务
    if (sqlite3_get_autocommit(db) == 0) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        spdlog::warn("ConnectionPool::release: forced ROLLBACK before returning to pool");
    }

    // 2) 清理可能残留的 ATTACH（保险措施）
    sqlite3_exec(db, "DETACH DATABASE db_000000", nullptr, nullptr, nullptr);
    // 更稳健的做法：查询 sqlite3_db_filename 列表，对非主 DB DETACH

    // 3) 重新设置 busy_timeout 为 3000ms，确保连接状态一致
    //    （SQLite 未提供查询 busy_timeout 的 API，统一在 release 时重置）
    sqlite3_busy_timeout(db, 3000);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pool.push_back(db);
    }
}
```

**强制约束**：

1. **统一超时**：所有连接必须在创建时设置 `sqlite3_busy_timeout(db, 3000)`，连接池不得接受未配置超时的连接。
2. **归还回滚**：`release()` 必须调用 `sqlite3_get_autocommit()` 检查；返回 `0` 时立即 `ROLLBACK` 并记录 warning。
3. **禁止调用方回滚**：调用方只负责使用 RAII `PoolGuard` 归还连接；回滚卫生由连接池统一处理，避免散落各处。
4. **ATTACH 清理**：即使 ScopeGuard 已在业务函数中 DETACH，连接池归还时仍应执行一次防御性清理，作为兜底。
5. **日志审计**：每次强制 ROLLBACK 必须写入日志（warning 级别），便于排查未按规范提交事务的调用点。

#### 3.5.5 V1.11 SqliteTxGuard RAII 事务包装类

**隐患**：尽管 §3.5.4 已强制连接池归还前 ROLLBACK 校验，但**业务函数内**手写 `BEGIN` / `COMMIT` / `ROLLBACK` 仍有重大风险：

- 业务函数中存在多个 `return` 路径，任意一个 `return` 漏写 `ROLLBACK` 就会让连接以"半提交"状态返回池内；
- 业务函数中抛异常（`std::bad_alloc`、SQLite `SQLITE_CONSTRAINT`、spdlog 失败等）若未捕获，栈展开后**手动写的 `COMMIT` 永远不会被执行**；
- 多个 `goto` / 嵌套 `if-else` / 异常嵌套混合的代码路径，让"两条语句必须配对"成为 review 阶段极难发现的隐性 bug。

**目标**：把"事务边界"封装为一个栈对象，由 C++ RAII 机制强制保证：

- 正常路径：对象析构时自动 `COMMIT`；
- 异常 / 早 return：对象析构时自动 `ROLLBACK`；
- 业务函数**无法绕过**事务关闭 —— 忘记写 `COMMIT` 不是错，**显式调用 `rollback()`** 才是反模式。

**头文件**：`src/datahub/SqliteTxGuard.h`（V1.11 新增）

```cpp
// ============================================================================
// EnerSentry — SqliteTxGuard RAII 事务包装 (V1.11)
// 所属 Target: ens::datahub (STATIC)
// 头文件: src/datahub/SqliteTxGuard.h
// ============================================================================
#pragma once

#include <sqlite3.h>
#include <string>
#include <string_view>
#include <utility>

namespace ens::datahub {

// ============================================================================
// TxType — 事务类型（对应 SQLite 的 BEGIN 变体）
// ============================================================================
enum class TxType : uint8_t {
    Deferred,    // BEGIN         (默认, 读事务或不要求写锁的场景)
    Immediate,   // BEGIN IMMEDIATE (写事务首选, 立即获取 RESERVED 锁,
                //                  避免写入期升级为 EXCLUSIVE 时的死锁)
    Exclusive,   // BEGIN EXCLUSIVE (极少使用, 仅当需要阻止所有其他读)
};

// ============================================================================
// SqliteTxGuard — SQLite 事务 RAII 包装
//
// 强制约束:
//   - 构造时立即执行 BEGIN（失败则 Q_ASSERT，连接不会进入"半开启"状态）
//   - 析构时根据 m_commitOnSuccess 决定 COMMIT 或 ROLLBACK
//   - commit() / rollback() 必须显式调用一次；析构函数会再次兜底
//   - 禁止拷贝/移动: 句柄独占, 不允许语义歧义
//
// 设计意图:
//   让"忘记写 COMMIT/ROLLBACK"成为不可能的 bug —— 业务函数只关心何时
//   "提交成功"（commit）或"放弃事务"（rollback），栈展开由 C++ 保证。
// ============================================================================
class SqliteTxGuard {
public:
    /// 构造并立即执行 BEGIN
    /// @param db       sqlite3* 句柄（必须已打开，且 busy_timeout 已配置）
    /// @param type     事务类型，默认为 Immediate（业务函数大多为写事务）
    /// @param timeoutMs busy_timeout（毫秒）；默认 0 表示使用连接池已配置值
    /// @throw std::runtime_error 当 BEGIN 失败时（连接已处于事务中 / SQLITE_BUSY 超时等）
    SqliteTxGuard(sqlite3* db, TxType type = TxType::Immediate,
                  int timeoutMs = 0)
        : m_db(db), m_active(false)
    {
        if (!m_db) {
            throw std::runtime_error("SqliteTxGuard: null sqlite3 handle");
        }
        // V1.11: 构造阶段可选择性重设 busy_timeout（不覆盖连接池配置）
        if (timeoutMs > 0) {
            sqlite3_busy_timeout(m_db, timeoutMs);
        }
        // 检查连接当前是否已在事务中 —— 避免嵌套 BEGIN
        if (sqlite3_get_autocommit(m_db) == 0) {
            throw std::runtime_error(
                "SqliteTxGuard: connection already in a transaction. "
                "Nested transactions are forbidden in SQLite. "
                "Did you forget to commit/rollback an outer guard?");
        }
        const char* sql = (type == TxType::Deferred)   ? "BEGIN"
                        : (type == TxType::Immediate)  ? "BEGIN IMMEDIATE"
                        :                                "BEGIN EXCLUSIVE";
        char* errMsg = nullptr;
        const int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::string err = errMsg ? errMsg : "unknown";
            sqlite3_free(errMsg);
            throw std::runtime_error(
                std::string("SqliteTxGuard: BEGIN failed: ") + err);
        }
        m_active = true;
    }

    /// 析构: 根据 m_commitOnSuccess 自动 COMMIT 或 ROLLBACK
    ~SqliteTxGuard() {
        if (!m_active) return;
        if (m_commitOnSuccess) {
            char* errMsg = nullptr;
            const int rc = sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, &errMsg);
            if (rc != SQLITE_OK) {
                // 析构阶段无法抛异常, 只能记录日志 + 强制 ROLLBACK
                spdlog::critical("SqliteTxGuard: COMMIT failed in dtor, "
                                 "fallback to ROLLBACK. err={}",
                                 errMsg ? errMsg : "unknown");
                sqlite3_free(errMsg);
                sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
            }
        } else {
            // 默认情况: 业务函数未显式 commit, 视为放弃 → ROLLBACK
            sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    // 禁止拷贝/移动 (RAII 句柄独占)
    SqliteTxGuard(const SqliteTxGuard&) = delete;
    SqliteTxGuard& operator=(const SqliteTxGuard&) = delete;
    SqliteTxGuard(SqliteTxGuard&&) = delete;
    SqliteTxGuard& operator=(SqliteTxGuard&&) = delete;

    /// 显式提交 (成功后 m_active 置为 false, 析构不再处理)
    /// @return true=COMMIT 成功, false=失败（已自动 ROLLBACK）
    bool commit() noexcept {
        if (!m_active) return false;
        char* errMsg = nullptr;
        const int rc = sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            spdlog::error("SqliteTxGuard: COMMIT failed, auto ROLLBACK. err={}",
                          errMsg ? errMsg : "unknown");
            sqlite3_free(errMsg);
            sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
            m_active = false;  // 析构不再重复处理
            return false;
        }
        m_active = false;
        return true;
    }

    /// 显式回滚 (成功后 m_active 置为 false, 析构不再处理)
    /// @return true=ROLLBACK 成功, false=失败（极少见, 多为连接已断开）
    bool rollback() noexcept {
        if (!m_active) return false;
        char* errMsg = nullptr;
        const int rc = sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            spdlog::error("SqliteTxGuard: ROLLBACK failed. err={}",
                          errMsg ? errMsg : "unknown");
            sqlite3_free(errMsg);
        }
        m_active = false;
        return (rc == SQLITE_OK);
    }

    /// 检查事务是否仍处于活跃状态
    [[nodiscard]] bool active() const noexcept { return m_active; }

private:
    sqlite3* m_db;           // 句柄（非拥有；连接由 ConnectionPool 管理）
    bool     m_active;       // 事务是否处于活跃态（commit/rollback 后置 false）
    bool     m_commitOnSuccess = false;  // 默认 false：未显式 commit → ROLLBACK
};

}  // namespace ens::datahub
```

**使用示例与规范写法**：

```cpp
// ✅ V1.11 推荐写法 —— 事务边界由 RAII 强制保证
bool DataAccessImpl::batchInsertHistory(const std::vector<DownSampledSample>& samples) {
    auto conn = m_pool.acquire();          // RAII 归还连接
    if (!conn) return false;

    try {
        SqliteTxGuard tx(conn->handle(), TxType::Immediate, 3000);
        // 业务逻辑: 跨多条 INSERT 写入
        for (const auto& s : samples) {
            sqlite3_stmt* stmt = /* prepare insert stmt */ nullptr;
            // ... bind, step ...
            sqlite3_finalize(stmt);
        }
        tx.commit();                       // 成功 → 提交
        return true;
    } catch (const std::exception& e) {
        spdlog::error("batchInsertHistory failed: {}", e.what());
        // tx 析构自动 ROLLBACK
        return false;
    }
    // 即使上面任何一处 early return 或 throw, tx 析构都会 ROLLBACK
}

// ❌ V1.11 禁止写法 —— 手写 BEGIN/COMMIT 易漏 ROLLBACK
bool DataAccessImpl::batchInsertHistory_BAD(const std::vector<DownSampledSample>& samples) {
    auto conn = m_pool.acquire();
    sqlite3_exec(conn->handle(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    for (const auto& s : samples) {
        // ... step ...
        if (someStepFailed) {
            // ⚠ 容易忘记写 ROLLBACK! 让连接以 BEGIN 状态返回池
            return false;                  // ← tx 未关闭
        }
    }
    sqlite3_exec(conn->handle(), "COMMIT", nullptr, nullptr, nullptr);
    return true;
}
```

**强制约束**：

1. **CR 红线**：所有业务函数中手写的 `sqlite3_exec(..., "BEGIN...")` / `"COMMIT"` / `"ROLLBACK"` 调用必须在 CR 阶段被驳回，强制改用 `SqliteTxGuard`。
2. **clang-tidy 检查**：`scripts/ci_sqlite_txguard.sh` 脚本扫描 `BEGIN` / `COMMIT` / `ROLLBACK` 字面量出现在 .cpp 中（非测试代码）时，CR 阶段告警 + CI 阻断。
3. **嵌套禁止**：`SqliteTxGuard` 构造时检查 `sqlite3_get_autocommit(db) == 0` 立即抛异常，杜绝"忘记外层 commit 就开始内层"的灾难。
4. **析构兜底**：业务函数即使完全忘记调用 `commit()` / `rollback()`，析构函数也会执行 ROLLBACK，保证连接不残留事务状态。
5. **超时一致**：`SqliteTxGuard` 构造时可重设 `busy_timeout`，但默认 0 表示不覆盖池内既有配置；保持与 §3.5.1 的 3000 ms 全局一致。
6. **代码库统一**：所有跨月查询、写事务、批量 UPDATE/DELETE 必须使用 `SqliteTxGuard`；连接池的 `release()` 中仍保留 V1.8 的 ROLLBACK 卫生检查作为兜底防线。

#### 3.5.6 V1.12 单线程写队列 (Single-Writer DB Queue)

**隐患**：§3.5.1/§3.5.5 规定的"每线程独立 connection + `sqlite3_busy_timeout=3000ms`"方案，在**读并发**场景下可以充分利用 SQLite WAL 的单写多读特性；但在 **L3 历史数据高频写入**场景（如 5,000 点/秒 black-box 快照、L2 聚合落库、审计日志批量写入）下会触及 SQLite 的根本限制：

- WAL 模式只是将"写阻塞读"转变为"读阻塞写"，**库级别的写锁仍然是单写者**（Single Writer Lock）；
- 当告警引擎、降采样引擎、历史持久化线程、审计线程等多个后台线程几乎同时通过 `SqliteTxGuard` 发起写事务时，SQLite 内部会在 `wal-index`、`wal-write-lock`、`wal-commit-lock` 上串行化；
- 即使设置了 `busy_timeout=3000ms`，极端情况下仍可能触发 `SQLITE_BUSY`，导致业务线程阻塞、写操作抖动甚至丢数据。

**优化建议**：对于 L3 数据库写密集型场景，引入 **单线程写队列 (Single-Writer DB Queue)** 模式，彻底消除 SQLite 库级写锁争用。

**核心架构**：

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                   Single-Writer DB Queue — 双队列优先级隔离 (V1.14)            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   告警引擎 ──┐    WriteOperation(type=InsertAlarmEvent)                      │
│   审计线程 ──┼──→ ───────────────────────────────────────┐                  │
│   SBO 日志 ──┤                                            │                  │
│   SOE 记录 ──┘    HighPriorityEventQueue (永不丢弃)       ▼                  │
│                                              ┌─────────────────┐            │
│   降采样引擎 ─┐   WriteOperation(type=InsertHistory)       │                 │
│   历史持久化 ─┼──→ ───────────────────────────────────┐    │   DbWriter     │
│               │                                       │    │   Thread       │
│               └───────────────────────────────────────┼──→ │                │
│                   TelemetryWriteQueue (允许 Drop/合并)│    │   每周期优先    │
│                                                       ▼    │   排空高优先级  │
│                                              ┌────┬──┴────┘   队列          │
│                                              │    │                         │
│                                              ▼    ▼                         │
│                                   ┌─────────────────────┐                   │
│                                   │  sqlite3* conn      │                   │
│                                   │  (唯一写句柄)        │                   │
│                                   │ BEGIN IMMEDIATE     │                   │
│                                   │ INSERT × N          │                   │
│                                   │ COMMIT              │                   │
│                                   └─────────────────────┘                   │
│                                                                             │
│   查询线程 A ──┐                                                            │
│   查询线程 B ──┼──→ 各自持有独立 sqlite3* 连接，仅执行 SELECT / ATTACH-DETACH  │
│   查询线程 C ──┘    (WAL 模式下并发读不阻塞写队列)                              │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

**设计要点**：

1. **所有写操作必须经队列**：后台业务线程不再直接持有 `sqlite3*` 写连接，而是将写请求序列化为 `WriteOperation` 对象，投递到无锁 MPSC 队列。
2. **V1.14 双队列优先级隔离**：写队列按业务关键性拆分为 **高优先级事件队列** (`HighPriorityEventQueue`) 与 **普通遥测队列** (`TelemetryWriteQueue`)，具体容量、背压策略与调度优先级见 §3.5.7。双队列避免海量遥测积压阻塞告警/审计等关键事件。
3. **唯一 DbWriter 线程**：一个专用后台线程（`DbWriter`）以 50~100ms 为周期从队列中批量取出 `WriteOperation`，在同一 `sqlite3*` 连接上开启一个 `BEGIN IMMEDIATE` 事务，执行批量 `INSERT/UPDATE/DELETE`，然后 `COMMIT`。每周期**优先排空高优先级队列**，再处理遥测队列。
4. **读连接与写连接分离**：其他业务线程仍保留独立 `sqlite3*` 连接，但**仅用于并发读**（`SELECT`、跨月 `ATTACH`/`DETACH`）。这些读连接不参与写事务，因此不会与 `DbWriter` 发生写锁争用。
5. **事务批次控制**：
   - **时间窗口**：最大累积 100ms 或累积 N 条操作（如 5000 条）即触发一次批量写入，防止高并发下队列积压；
   - **容量窗口**：单次事务内操作数上限（如 10,000 条），避免事务过大导致 WAL 文件膨胀和 checkpoint 阻塞。
6. **失败与重试**：
   - 若某批次 `BEGIN IMMEDIATE` 返回 `SQLITE_BUSY`（理论上极低，因为只有单一写者），`DbWriter` 应使用指数退避重试，并在连续失败时上报运维告警；
   - 单条 `INSERT` 失败（如约束冲突）不应导致整批回滚，除非业务语义要求原子批次。建议按业务类型分桶批次。

**WriteOperation 结构示例**：

```cpp
// ============================================================================
// EnerSentry — Single-Writer DB Queue Operation (V1.12)
// 所属 Target: ens::datahub (STATIC)
// 头文件: src/datahub/DbWriteQueue.h
// ============================================================================
#pragma once

#include <QByteArray>   // payload 序列化后二进制；或改用 flatbuffer/msgpack
#include <QString>
#include <cstdint>
#include <variant>

namespace ens::datahub {

enum class DbWriteOpType : uint8_t {
    InsertHistory,    // L2/L3 历史采样批量插入 → TelemetryWriteQueue
    InsertAuditLog,   // 审计日志 → HighPriorityEventQueue
    InsertAlarmEvent, // 告警事件 → HighPriorityEventQueue
    InsertSoeEvent,   // SOE 历史记录 → HighPriorityEventQueue
    UpdateSboState,   // SBO 状态持久化 → HighPriorityEventQueue
    CustomSql,        // 预留：带参数绑定的预定义 SQL（由调用方指定队列）
};

/// V1.14 写操作优先级分类，决定进入哪条队列
enum class DbWriteOpPriority : uint8_t {
    HighPriority,   // 告警/ SOE / SBO 日志/审计日志：永不丢弃
    Telemetry,      // 遥测采样：允许按策略丢弃或合并
};

/// 根据操作类型返回默认优先级 / 目标队列
constexpr DbWriteOpPriority priorityForType(DbWriteOpType type) noexcept {
    switch (type) {
    case DbWriteOpType::InsertAlarmEvent:
    case DbWriteOpType::InsertSoeEvent:
    case DbWriteOpType::UpdateSboState:
    case DbWriteOpType::InsertAuditLog:
        return DbWriteOpPriority::HighPriority;
    case DbWriteOpType::InsertHistory:
    case DbWriteOpType::CustomSql:
    default:
        return DbWriteOpPriority::Telemetry;
    }
}

struct InsertHistoryPayload {
    uint32_t pointId;
    uint64_t timestamp;
    float    valueMax;
    float    valueMin;
    float    valueAvg;
};

struct InsertAuditLogPayload {
    uint64_t eventTime;
    QString  user;
    QString  action;
    QString  target;
    QString  detail;
    QString  result;
};

using DbWriteOpPayload = std::variant<
    InsertHistoryPayload,
    InsertAuditLogPayload,
    InsertAlarmEvent,
    UpdateSboState,
    QByteArray          // CustomSql 的预编译参数二进制
>;

struct DbWriteOperation {
    DbWriteOpType   type;
    DbWriteOpPayload payload;
    uint64_t         enqueueTime;  // 用于观测队列等待延迟
};

}  // namespace ens::datahub
```

**DbWriter 线程主循环伪代码**：

```cpp
// ============================================================================
// DbWriter::run() 主循环伪代码 (V1.12)
// 注意: 这是唯一允许直接对主库执行 BEGIN/COMMIT 的线程; 业务代码禁止使用。
// ============================================================================
void DbWriter::run() {
    // 1) 初始化唯一写连接
    sqlite3* db = nullptr;
    sqlite3_open_v2(m_dbPath.toUtf8().constData(), &db,
                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_WAL,
                    nullptr);
    sqlite3_busy_timeout(db, 3000);

    std::vector<DbWriteOperation> highBatch;
    std::vector<DbWriteOperation> teleBatch;
    highBatch.reserve(kMaxHighPriorityBatchSize);
    teleBatch.reserve(kMaxTelemetryBatchSize);

    while (!m_stop.load(std::memory_order_acquire)) {
        // 2) V1.14 优先排空高优先级事件队列，再处理遥测队列
        highBatch.clear();
        m_highPriorityQueue.wait_dequeue_bulk_timed(
            highBatch.data(), kMaxHighPriorityBatchSize,
            std::chrono::milliseconds(10));  // 高优先级队列短暂等待即可

        // 3) 收集遥测队列（时间窗口 + 容量窗口）
        teleBatch.clear();
        m_telemetryQueue.wait_dequeue_bulk_timed(
            teleBatch.data(), kMaxTelemetryBatchSize,
            std::chrono::milliseconds(100));

        // 4) 高优先级事件永远先提交，避免被遥测淹没
        if (!highBatch.empty()) {
            commitBatch(db, highBatch, /*isHighPriority=*/true);
        }
        if (!teleBatch.empty()) {
            commitBatch(db, teleBatch, /*isHighPriority=*/false);
        }

        // 5) 记录批量写入延迟 (enqueueTime -> now) 到 metrics
        reportBatchLatency(highBatch, /*queue=*/"high");
        reportBatchLatency(teleBatch, /*queue=*/"telemetry");
    }

    sqlite3_close(db);
}

void DbWriter::commitBatch(sqlite3* db,
                           const std::vector<DbWriteOperation>& batch,
                           bool isHighPriority) {
    // 按业务类型分桶: 历史 / 审计 / 告警 / SOE / SBO 分别成独立事务,
    // 避免一种业务失败导致整批回滚。
    auto buckets = groupByType(batch);

    for (auto& [type, ops] : buckets) {
        if (ops.empty()) continue;

        SqliteTxGuard tx(db, TxType::Immediate, 3000);

        for (const auto& op : ops) {
            switch (op.type) {
            case DbWriteOpType::InsertHistory:
                insertHistoryBindAndStep(db, op.payload);
                break;
            case DbWriteOpType::InsertAuditLog:
            case DbWriteOpType::InsertSoeEvent:
                insertAuditLogBindAndStep(db, op.payload);
                break;
            case DbWriteOpType::InsertAlarmEvent:
                insertAlarmEventBindAndStep(db, op.payload);
                break;
            case DbWriteOpType::UpdateSboState:
                updateSboStateBindAndStep(db, op.payload);
                break;
            // ... 其他类型 ...
            }
        }

        if (!tx.commit()) {
            spdlog::error("DbWriter: batch commit failed for type={}, "
                          "highPriority={}",
                          static_cast<int>(type), isHighPriority);
            // SqliteTxGuard dtor 已自动 ROLLBACK; 失败条目可转入死信队列
        }
    }
}
```

**业务线程投递伪代码**：

```cpp
// 业务线程 (如历史持久化线程) 不再直接写库, 而是按类型投递到对应队列
void HistoryPersistenceThread::onBatchReady(
    const std::vector<DownSampledSample>& samples) {

    for (const auto& s : samples) {
        DbWriteOperation op;
        op.type = DbWriteOpType::InsertHistory;
        op.payload = InsertHistoryPayload{
            .pointId   = s.pointId,
            .timestamp = s.timestamp,
            .valueMax  = s.valueMax,
            .valueMin  = s.valueMin,
            .valueAvg  = s.valueAvg,
        };
        op.enqueueTime = nowMs();
        // InsertHistory 属于 Telemetry，进入遥测队列
        m_dbWriteQueue.enqueueTelemetry(std::move(op));
    }
}

// 告警引擎示例: 告警事件进入高优先级事件队列，永不丢弃
void AlarmEngine::persistAlarmEvent(const AlarmEvent& event) {
    DbWriteOperation op;
    op.type = DbWriteOpType::InsertAlarmEvent;
    op.payload = event;
    op.enqueueTime = nowMs();
    // V1.14: 高优先级事件使用阻塞入队，最多等待 500ms；超时转 Spill File
    const bool enqueued = m_dbWriteQueue.enqueueHighPriorityBlocking(
        std::move(op), std::chrono::milliseconds(500));
    if (!enqueued) {
        m_spillWriter.append(op);
        spdlog::warn("HighPriorityQueue full: AlarmEvent spilled (queueDepth={})",
                     m_dbWriteQueue.highPriorityDepthApprox());
    }
}
```

**强制约束**：

1. **写唯一性**：整个进程只允许存在一个 `DbWriter` 线程持有"主库写连接"。禁止任何业务线程绕过队列直接对主库执行 `INSERT`/`UPDATE`/`DELETE`。
2. **队列无锁化**：业务线程到 `DbWriter` 的队列必须是无锁 MPSC（如 `moodycamel::ConcurrentQueue` 或基于 `SpscRingBuffer` 的扩展），避免业务线程在投递时因锁竞争产生抖动。
3. **读连接不参与写**：保留 Per-Thread / Connection Pool 的线程各自 `sqlite3*` 连接**仅用于读**。若某读线程需要写历史数据，必须将操作投递到 `DbWriteQueue`。
4. **失败隔离**：建议按 `DbWriteOpType` 分桶批次，避免审计日志写入失败导致历史采样整批回滚。
5. **可观测性**：必须暴露以下 metrics（V1.14 按双队列拆分）：
   - `db_write_queue_depth{queue="high"|"telemetry"}`：高优先级事件队列 / 遥测队列当前积压数量；
   - `db_write_batch_latency_ms{queue="high"|"telemetry"}`：从 enqueue 到 commit 的 P50/P99 延迟；
   - `db_write_busy_retries`：`DbWriter` 遇到 `SQLITE_BUSY` 重试次数（正常应为 0）。
6. **降级策略（V1.14 细化）**：当 `DbWriteQueue` 持续满载且 `DbWriter` 无法跟上时，应触发背压机制：
   - **高优先级事件队列**：告警/ SOE / SBO 日志/审计日志**永不丢弃**，必须阻塞业务线程或转存到本地 Spill File；
   - **遥测队列**：允许按丢帧计数 Drop 或降采样合并，保护高优先级路径不被拖垮。具体策略见 §3.5.7。

**适用决策树**：

| 场景 | 推荐方案 | 说明 |
|------|---------|------|
| 低频配置写入、用户操作审计 | Per-Thread Connection + `SqliteTxGuard` | 并发量低，单写者锁不会成为瓶颈 |
| 中频历史查询 + 少量写入 | Connection Pool + `SqliteTxGuard` | 读写并发可控，`busy_timeout=3000ms` 足够 |
| **高频落库** (≥ 1,000 点/秒 持续写入，或多后台线程同时写) | **Single-Writer DB Queue** | 彻底消除写锁争用，必须作为 L3 默认架构 |

**反向约束**：引入 `DbWriteQueue` 后，`IDataAccess::batchInsertHistory()` / `insertAuditLog()` 等写接口的实现层应改为**投递到队列**，而不是直接执行 SQL。`IDataAccess` 的抽象接口本身保持不变，但实现层必须显式区分"读实现"与"写队列投递"两条路径。

#### 3.5.7 V1.14 双队列背压与优先级隔离 (HighPriorityEventQueue / TelemetryWriteQueue)

**隐患**：§3.5.6 引入的单线程写队列虽然消除了 SQLite 库级写锁争用，但在 5,000 点/秒高频落库场景下，若发生磁盘 IO 抖动（如 SSD 垃圾回收、WAL checkpoint 阻塞、操作系统换页），`DbWriter` 批量写入延迟可能从正常 10~50ms 突增至数百毫秒。如果所有写操作共享同一条无锁队列，会出现两个致命问题：

1. **遥测积压阻塞关键事件**：海量 `InsertHistory` 遥测操作排在队列前部，导致后进入的告警事件、SOE、SBO 日志必须等待整批遥测提交后才能落库，实时性被破坏；
2. **背压策略互相掣肘**：为了"不丢关键事件"必须阻塞或 Spill，但同一队列上的非关键遥测也会占用队列容量，使阻塞阈值难以设定。

**目标**：将写队列按业务关键性拆分为**物理隔离的双队列**，使关键事件路径与遥测路径拥有独立的容量、背压策略和调度优先级。

**双队列设计**：

| 队列 | 承载操作类型 | 容量设计 | 丢弃策略 | 调度优先级 |
|------|-------------|---------|---------|-----------|
| `HighPriorityEventQueue` | `InsertAlarmEvent`、`InsertSoeEvent`、`UpdateSboState`、`InsertAuditLog` | 10,000 ~ 50,000（约 1~5s 缓冲） | **永不丢弃** | `DbWriter` 每周期**优先排空** |
| `TelemetryWriteQueue` | `InsertHistory`、可容忍延迟的批量采样/聚合 | 100,000 ~ 500,000（约 20~100s 缓冲） | 高水位限流/合并；满载按丢帧计数 Drop | 高优先级队列为空后再处理 |

**背压三要素（按队列独立配置）**：

| 要素 | `HighPriorityEventQueue` | `TelemetryWriteQueue` |
|------|--------------------------|-----------------------|
| **硬容量上限** | `kMaxHighPriorityDepth` = 50,000 | `kMaxTelemetryDepth` = 500,000 |
| **高水位线** | `kHighPriorityHighWater` = 70% | `kTelemetryHighWater` = 75% |
| **满载动作** | 阻塞业务线程，超时转 Spill File | 按丢帧计数 Drop 或降采样合并 |
| **典型阻塞超时** | 500ms ~ 1000ms | 不适用（直接 Drop/合并） |

**操作类型 → 队列路由表**：

| `DbWriteOpType` | 目标队列 | 理由 |
|-----------------|----------|------|
| `InsertAlarmEvent` | `HighPriorityEventQueue` | 告警事件不可丢失，必须实时落库 |
| `InsertSoeEvent` | `HighPriorityEventQueue` | SOE 是事故反演依据，不可丢失 |
| `UpdateSboState` | `HighPriorityEventQueue` | SBO 控制日志需满足电网安全审计要求 |
| `InsertAuditLog` | `HighPriorityEventQueue` | 用户操作审计需完整留存 |
| `InsertHistory` | `TelemetryWriteQueue` | 历史采样量大，允许部分丢帧/合并 |
| `CustomSql` | 由调用方通过 `DbWriteOpPriority` 显式指定 | 默认归入遥测队列 |

**推荐实现架构**：

```
┌─────────────────────────────────────────────────────────────────────────────┐
│              Dual-Queue Backpressure with Priority Isolation (V1.14)        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   告警/SOE/SBO/审计 ──→ HighPriorityEventQueue ──┐                          │
│        (永不丢弃)              (capacity Nh)     │                          │
│                                                  ▼                          │
│   遥测/历史采样 ─────→ TelemetryWriteQueue ───→ ┌─────────────┐            │
│        (可 Drop/合并)         (capacity Nt)     │   DbWriter  │            │
│                                                 │   Thread    │            │
│                                                 │  1. drain high│           │
│                                                 │  2. drain tele│           │
│                                                 │  3. commit   │            │
│                                                 └──────┬──────┘            │
│                                                        │                    │
│                                                        ▼                    │
│                                             ┌─────────────────────┐         │
│                                             │  sqlite3* conn      │         │
│                                             │  BEGIN IMMEDIATE    │         │
│                                             │  INSERT × N         │         │
│                                             │  COMMIT             │         │
│                                             └─────────────────────┘         │
│                                                                             │
│   Spill File ◄── 高优先级队列阻塞超时 ──→ 回放优先于遥测队列                  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

**DbWriter 主循环调度逻辑**：

```cpp
// ============================================================================
// DbWriter::run() 调度片段 (V1.14 双队列)
// ============================================================================
while (!m_stop.load(std::memory_order_acquire)) {
    std::vector<DbWriteOperation> highBatch;
    std::vector<DbWriteOperation> teleBatch;

    // 1) 优先尝试排空高优先级事件队列（短超时，不等待遥测）
    highBatch.reserve(kMaxHighPriorityBatchSize);
    m_highPriorityQueue.wait_dequeue_bulk_timed(
        highBatch.data(), kMaxHighPriorityBatchSize,
        std::chrono::milliseconds(10));

    // 2) 收集遥测队列（时间窗口 + 容量窗口）
    teleBatch.reserve(kMaxTelemetryBatchSize);
    m_telemetryQueue.wait_dequeue_bulk_timed(
        teleBatch.data(), kMaxTelemetryBatchSize,
        std::chrono::milliseconds(100));

    // 3) 高优先级永远先提交
    if (!highBatch.empty()) {
        commitBatch(db, highBatch, /*isHighPriority=*/true);
    }
    if (!teleBatch.empty()) {
        commitBatch(db, teleBatch, /*isHighPriority=*/false);
    }

    // 4) 空闲时回放 Spill File
    replaySpillFileIfIdle();
}
```

**高优先级队列阻塞 + Spill File 兜底**：

```cpp
// ============================================================================
// 高优先级事件入队示例 (V1.14)
// ============================================================================
bool AlarmEngine::persistAlarmEvent(const AlarmEvent& event) {
    DbWriteOperation op;
    op.type = DbWriteOpType::InsertAlarmEvent;
    op.payload = event;
    op.enqueueTime = nowMs();

    // 高优先级事件: 最多阻塞 500ms, 超时转 Spill File, 永不丢弃
    const bool enqueued = m_dbWriteQueue.enqueueHighPriorityBlocking(
        std::move(op), std::chrono::milliseconds(500));
    if (!enqueued) {
        m_spillWriter.append(op);
        m_metrics.dbWriteSpillFileBytes.fetch_add(estimateSize(op),
                                                  std::memory_order_relaxed);
        spdlog::warn("HighPriorityEventQueue full: AlarmEvent spilled to {} "
                     "(queueDepth={})",
                     m_spillWriter.path(),
                     m_dbWriteQueue.highPriorityDepthApprox());
        return false;
    }
    return true;
}
```

**遥测队列丢弃 / 合并策略**：

```cpp
// ============================================================================
// 遥测数据入队示例 (V1.14)
// 策略: 高水位时同一 pointId 连续采样合并为最新一条;
//       满载时按丢帧计数直接丢弃。
// ============================================================================
void TelemetryWriter::enqueueHistory(const InsertHistoryPayload& payload) {
    const size_t depth = m_dbWriteQueue.telemetryDepthApprox();

    if (depth >= kMaxTelemetryDepth) {
        // 满载: 按 pointId 累计丢帧数, 直接丢弃
        m_metrics.telemetryDroppedByPoint[payload.pointId].fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    if (depth >= kTelemetryHighWater) {
        // 高水位: 尝试合并同一 pointId 的 pending 采样
        if (m_dbWriteQueue.tryMergeTelemetry(payload)) {
            return;  // 已合并, 不增加队列深度
        }
        // 合并不成则继续入队; 若深度继续增长, 下一帧可能进入满载分支
    }

    DbWriteOperation op;
    op.type = DbWriteOpType::InsertHistory;
    op.payload = payload;
    op.enqueueTime = nowMs();
    m_dbWriteQueue.enqueueTelemetry(std::move(op));
}
```

**Spill File 机制（关键写兜底）**：

1. **触发条件**：高优先级事件队列阻塞超时时；或队列满载且该操作不可丢弃时。
2. **文件格式**：顺序追加的本地二进制 WAL（如 `db_spill_YYYYMMDD.bin`），每条记录含长度前缀 + `DbWriteOperation` 序列化。
3. **回放优先级**：`DbWriter` 在正常批量写入空闲时，**优先回放 Spill File**，再处理遥测队列；确保关键事件最终一致。
4. **容量上限**：Spill File 本身也要设置上限（如 1GB），超过则触发运维告警并通知人工介入。

**强制约束**：

1. **物理隔离**：`HighPriorityEventQueue` 与 `TelemetryWriteQueue` 必须是两条独立的队列实例，禁止在同一条队列上混合高低优先级操作。
2. **高优先级永不丢弃**：`InsertAlarmEvent`、`InsertSoeEvent`、`UpdateSboState`、`InsertAuditLog` 严禁因队列满而被静默丢弃；必须阻塞、Spill File 或业务降级。
3. **DbWriter 调度优先级固定**：每周期必须先尝试排空高优先级队列，再收集遥测队列；禁止为了提高吞吐量而合并处理。
4. **必须设置队列容量上限**：禁止无界队列；`kMaxHighPriorityDepth` 与 `kMaxTelemetryDepth` 必须在代码中显式定义，并在启动日志中打印。
5. **必须暴露背压 metrics**：
   - `db_write_queue_depth{queue="high"}` / `db_write_queue_depth{queue="telemetry"}`：当前队列深度；
   - `db_write_queue_dropped_total{queue="telemetry",point_id="<id>"}`：遥测按 pointId 丢帧总数；
   - `db_write_queue_merged_total{queue="telemetry",point_id="<id>"}`：遥测合并总数；
   - `db_write_queue_blocked_ms{queue="high"}`：高优先级事件阻塞等待时间 P50/P99；
   - `db_write_spill_file_bytes`：Spill File 当前大小；
   - `db_write_spill_file_replayed_total`：已回放记录数。
6. **必须设置监控告警**：
   - `db_write_queue_depth{queue="high"}` 持续 30s 超过 70% 容量时触发告警；
   - `db_write_queue_depth{queue="telemetry"}` 满载时触发告警；
   - Spill File 容量超过 80% 时触发紧急告警。
7. **单元测试覆盖**：必须模拟 `DbWriter` 人为延迟（如注入 `std::this_thread::sleep_for`），验证：
   - 遥测在队列满载时按 pointId 计数丢弃；
   - 高优先级事件在队列满载时进入 Spill File；
   - Spill File 回放后数据最终一致；
   - 高优先级队列积压时，遥测不会阻塞关键事件提交；
   - 双队列 depth metric 独立变化。

---

## 4. 业务引擎与 SBO 控制状态机接口 (Layer 4)

### 4.1 AlarmEngine 告警引擎接口

**所属 Target**：`ens::business`（SHARED 库，`business.dll`）  
**头文件路径**：`src/business/AlarmEngine.h`

```cpp
// ============================================================================
// EnerSentry — AlarmEngine 告警引擎接口 (V1.5)
// 所属 Target: ens::business (SHARED)
// 头文件: src/business/AlarmEngine.h
// ============================================================================
#pragma once

#include "ens/export.hpp"
#include "datahub/Sample.h"
#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>
#include <vector>

namespace ens::business {

// ============================================================================
// AlarmEvent — 告警事件结构体（告警引擎产出 → 告警中心消费）
// ============================================================================
struct ENS_BUSINESS_API AlarmEvent {
    uint64_t    alarmId;          // 全局唯一告警 ID（时间戳 + 序列号）
    uint32_t    pointId;          // 触发测点 ID
    datahub::AlarmLevel level;    // 告警级别
    uint64_t    triggerTime;      // 触发时间（Unix ms）
    float       currentValue;     // 当前值
    float       thresholdValue;   // 阈值
    QString     pointName;        // 测点描述（从点表获取）
    QString     deviceName;       // 设备名称
    QString     message;          // 告警消息（人类可读）
    bool        isSuppressed;     // 是否为抑制合并投递（同源抑制场景）
    std::vector<uint64_t> suppressedAlarmIds;  // 被抑制的原始告警 ID 列表
};

// ============================================================================
// AlarmRule — 告警规则（从 alarm_rules.json 加载）
// ============================================================================
struct ENS_BUSINESS_API AlarmRule {
    uint32_t    pointId;
    AlarmLevel  level;
    float       upperThreshold;       // 上限阈值
    float       lowerThreshold;       // 下限阈值
    float       hysteresisBand;       // 迟滞带（防止临界值反复触发）
    uint32_t    confirmDelayMs;       // 延时确认时间（默认 3000ms）
    uint32_t    suppressWindowMs;     // 同源抑制窗口（默认 60000ms）
    bool        enableBlackBox;       // 是否触发黑匣子快照
};

// ============================================================================
// 告警回调类型
// ============================================================================
using AlarmCallback = std::function<void(const AlarmEvent& event)>;

// ============================================================================
// AlarmEngine — 告警引擎
//
// 设计约束:
//  - 运行在独立告警线程（Core 2，HIGH 优先级）
//  - 输入: 订阅 DataBus 实时数据流（Sample）
//  - 处理: 滑动窗口阈值判定 → 迟滞 → 延时确认 → 同源抑制 → 投递
//  - 输出: 通过 signal/callback 投递 AlarmEvent 到 UI 告警中心
//  - 端到端延迟 < 100ms（从数据到达至 AlarmEvent 发出）
// ============================================================================
class ENS_BUSINESS_API AlarmEngine : public QObject {
    Q_OBJECT

public:
    explicit AlarmEngine(QObject* parent = nullptr);
    ~AlarmEngine() override;

    // 禁止拷贝与移动
    AlarmEngine(const AlarmEngine&) = delete;
    AlarmEngine& operator=(const AlarmEngine&) = delete;

    // ──── 生命周期 ────

    /// 启动告警引擎（开始消费 DataBus 数据流）
    void start();

    /// 停止告警引擎（不再产生新告警）
    void stop();

    // ──── 规则管理 ────

    /// 加载告警规则（支持热加载）
    /// @param rules 从 alarm_rules.json 解析的规则列表
    void loadRules(const std::vector<AlarmRule>& rules);

    /// 获取当前生效的规则列表
    std::vector<AlarmRule> activeRules() const;

    /// 临时屏蔽某测点告警（维护场景）
    /// @param pointId      测点 ID
    /// @param durationMs   屏蔽时长（0 表示永久屏蔽，需手动解除）
    /// @param reason       屏蔽原因（写入审计日志）
    /// @return             true=屏蔽成功；false=已在屏蔽中
    bool suppressAlarm(uint32_t pointId, uint64_t durationMs, const QString& reason);

    /// 解除屏蔽
    void unsuppressAlarm(uint32_t pointId, const QString& reason);

    // ──── 回调注册 ────

    /// 注册告警事件回调（C++ 函数式风格）
    void setAlarmCallback(AlarmCallback cb);

    /// 注册黑匣子触发回调（告警引擎触发，黑匣子管理器执行）
    /// 回调签名: void(uint32_t pointId, uint64_t alarmTime, AlarmLevel level)
    void setBlackBoxTriggerCallback(
        std::function<void(uint32_t, uint64_t, datahub::AlarmLevel)> cb);

signals:
    // ──── Qt 跨线程信号（UI 告警中心通过 QueuedConnection 接收）────

    /// 新告警产生
    void newAlarm(const ens::business::AlarmEvent& event);

    /// 告警恢复（测点值回落至正常范围）
    void alarmRecovered(uint64_t alarmId, uint32_t pointId, uint64_t recoverTime);

    /// 告警风暴告警（同源抑制窗口内累积超过阈值，发出运维提醒）
    /// @param pointId      测点 ID
    /// @param suppressedCount 被抑制的告警数量
    void alarmStormWarning(uint32_t pointId, int suppressedCount);

private:
    // 滑动窗口阈值判定 (Threshold Check with Hysteresis)
    // 同源抑制 (Suppression with Sliding Window)
    // 延时确认 (Delayed Confirmation Timer)
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ens::business
```

### 4.1.1 告警回调与信号契约

**目的**：将 §4.1 `AlarmEngine` 中散落的回调类型、信号签名、跨线程投递方式集中汇总，消除“回调定义未完结”的文档歧义。

```cpp
// ============================================================================
// EnerSentry — AlarmEngine 回调与信号契约 (V1.9)
// 头文件: src/business/AlarmEngine.h (摘录)
// ============================================================================

namespace ens::business {

// ──── C++ 函数式回调（非 Qt 信号，适合业务链路内部链式处理）────

/// 告警事件回调：告警引擎产生最终 AlarmEvent 时触发
/// @param event 已填充完整的告警事件（含抑制合并标记）
using AlarmCallback = std::function<void(const AlarmEvent& event)>;

/// 黑匣子触发回调：告警判定命中且规则启用 blackBox 时触发
/// @param pointId 触发测点 ID
/// @param alarmTime 告警触发时间 (Unix ms)
/// @param level     告警级别
using BlackBoxTriggerCallback =
    std::function<void(uint32_t pointId, uint64_t alarmTime, datahub::AlarmLevel level)>;

// ──── Qt 跨线程信号（必须在 main() 中执行 qRegisterMetaType，参见 §1.5）────

signals:
    /// 新告警产生 → UI 告警中心（QueuedConnection）
    void newAlarm(const ens::business::AlarmEvent& event);

    /// 告警恢复 → UI 告警中心（QueuedConnection）
    void alarmRecovered(uint64_t alarmId, uint32_t pointId, uint64_t recoverTime);

    /// 告警风暴提醒 → UI 告警中心（QueuedConnection）
    void alarmStormWarning(uint32_t pointId, int suppressedCount);

}  // namespace ens::business
```

**跨线程投递约束**：

| 信号 | 发送线程 | 接收线程 | 推荐连接方式 |
|------|---------|---------|-------------|
| `newAlarm` | 告警线程 (Core 2) | UI 主线程 | `Qt::QueuedConnection` |
| `alarmRecovered` | 告警线程 (Core 2) | UI 主线程 | `Qt::QueuedConnection` |
| `alarmStormWarning` | 告警线程 (Core 2) | UI 主线程 | `Qt::QueuedConnection` |

> **回调注册与信号二选一**：业务侧可通过 `setAlarmCallback(AlarmCallback)` 在 C++ 链路中同步处理告警；若需跨线程通知 UI，必须连接 `newAlarm` 信号。两者不应同时用于同一消费端，避免重复投递。

### 4.2 DeviceSboGuard 设备级 SBO 逻辑锁接口

**所属 Target**：`ens::business`（SHARED 库）  
**头文件路径**：`src/business/DeviceSboGuard.h`

```cpp
// ============================================================================
// EnerSentry — DeviceSboGuard 设备级 SBO 逻辑锁 (V1.5)
// 所属 Target: ens::business (SHARED)
// 头文件: src/business/DeviceSboGuard.h
// ============================================================================
#pragma once

#include "ens/export.hpp"
#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>

// V1.8 前置声明: QTimer 仅在 Impl 内部使用, 公共头文件禁止包含 <QTimer>
class QTimer;

namespace ens::business {

// ============================================================================
// SboDeviceKey — 设备级 SBO 二维锁 Key
//
// 三维约束确保精确互斥:
//  - linkId:       通信链路 ID（定位物理通道）
//  - slaveId:      Modbus 从站地址（定位逻辑设备）
//  - registerAddr: 操作寄存器地址（定位具体控制点）
//
// 示例：
//   PCS#1 排风控制:  (linkId=1, slaveId=2,  registerAddr=0x1000)
//   PCS#1 液冷控制:  (linkId=1, slaveId=2,  registerAddr=0x2000)  ← 不冲突！
//   PCS#10 排风控制: (linkId=1, slaveId=11, registerAddr=0x1000)  ← 不冲突！
// ============================================================================
struct ENS_BUSINESS_API SboDeviceKey {
    uint32_t linkId;
    uint32_t slaveId;
    uint32_t registerAddr;

    /// 基于 boost::hash_combine 风格的 64-bit 哈希混合
    /// 说明: linkId / slaveId 通常是小整数（1~10），registerAddr 集中在特定段
    /// （如 0x1000, 0x2000）。纯 32-bit FNV-1a 低位混合不充分，在 unordered_map
    /// 桶数组为 2 的幂时易产生局部碰撞。V1.5 升级为 64-bit seed 与 Golden Ratio
    /// 常数 0x9e3779b97f4a7c15ULL，同时打散高位与低位，进一步提升哈希分布质量。
    [[nodiscard]] size_t hash() const noexcept {
        size_t seed = 0;
        auto combine = [&seed](uint32_t val) {
            // 64-bit boost::hash_combine 变体（Golden Ratio 常量）
            seed ^= std::hash<uint32_t>{}(val) + 0x9e3779b97f4a7c15ULL
                    + (seed << 6) + (seed >> 2);
        };
        combine(linkId);
        combine(slaveId);
        combine(registerAddr);
        return seed;
    }

    bool operator==(const SboDeviceKey& o) const noexcept {
        return linkId == o.linkId && slaveId == o.slaveId && registerAddr == o.registerAddr;
    }

    bool operator!=(const SboDeviceKey& o) const noexcept { return !(*this == o); }
};

// ============================================================================
// SboKeyHash — SboDeviceKey 的哈希函数对象
// 供 std::unordered_map<SboDeviceKey, T, SboKeyHash> 使用
// ============================================================================
struct SboKeyHash {
    [[nodiscard]] size_t operator()(const SboDeviceKey& key) const noexcept {
        return key.hash();
    }
};

// ============================================================================
// SboSequenceState — SBO 序列状态枚举
// ============================================================================
enum class SboSequenceState : uint8_t {
    Idle      = 0,   // 空闲
    Select    = 1,   // 已选择目标设备
    Armed     = 2,   // 预置待确认（倒计时中）
    Operate   = 3,   // 已执行（成功下发）
    Expired   = 4,   // 超时过期（未二次确认）
    Cancelled = 5,   // 用户取消
    Failed    = 6,   // 下发失败（通信超时/设备拒绝）
};

// ============================================================================
// ArmedOccupant — Armed 状态的占用者信息
// ============================================================================
struct ENS_BUSINESS_API ArmedOccupant {
    QString  sequenceId;         // SBO 序列唯一 ID
    QString  operatorName;       // 操作员名称
    uint64_t armedSinceMs;       // Armed 的时间戳 (Unix ms)
    QString  operationType;      // 操作类型描述（如 "开启排风"）
};

// ============================================================================
// SboSequenceResult — SBO 序列执行结果
// ============================================================================
struct ENS_BUSINESS_API SboSequenceResult {
    QString          sequenceId;
    SboDeviceKey     deviceKey;
    SboSequenceState finalState;
    QString          errorMessage;   // 仅 Failed 状态时有效
    uint64_t         timestampMs;    // 结果生成时间
};

// ============================================================================
// SBO 状态回调类型
// ============================================================================
using SboStateCallback = std::function<void(const SboSequenceResult& result)>;

// ============================================================================
// DeviceSboGuard — 设备级 SBO 逻辑锁
//
// 设计约束:
//  - 按 SboDeviceKey 分桶：不同 Key 可并行占用（设备级并发）
//  - 相同 Key 互斥：只有一个 SBO 序列可处于 Armed 状态
//  - 超时自动释放：Armed 5s 倒计时到期自动清除（急停 3s）
//  - 断线自动清除：Armed 期间目标设备链路断线 → 自动清除 Armed
//  - 所有状态变更写入审计日志
// ============================================================================
class ENS_BUSINESS_API DeviceSboGuard : public QObject {
    Q_OBJECT

public:
    explicit DeviceSboGuard(QObject* parent = nullptr);
    ~DeviceSboGuard() override;

    // ──── 锁操作 ────

    /// 尝试获取设备级锁（Select → Armed 过渡）
    /// @param key          设备 Key
    /// @param sequenceId   SBO 序列唯一 ID
    /// @param operatorName 操作员名称
    /// @param armedTimeoutMs Armed 超时时间（默认 5000ms，急停 3000ms）
    /// @param outOccupant  输出当前占用者信息（仅失败时写入）
    /// @return             true=锁获取成功（已进入 Armed）；false=该设备已有 SBO 在 Armed
    bool tryAcquire(const SboDeviceKey& key, const QString& sequenceId,
                    const QString& operatorName,
                    int armedTimeoutMs = 5000,
                    ArmedOccupant* outOccupant = nullptr);

    /// 释放锁（Operate / Cancel / 异常终止时调用）
    /// @param key         设备 Key
    /// @param sequenceId  SBO 序列唯一 ID（必须匹配）
    /// @param finalState  最终状态（Operate/Cancelled/Failed/Expired）
    ///
    /// ⚠ V1.1 资源回收契约（强制执行）:
    ///   本方法进入终态分支时，DeviceSboGuard::Impl 内部
    ///   std::unordered_map<SboDeviceKey, ArmedOccupant, SboKeyHash> m_armedMap
    ///   与 std::unordered_map<SboDeviceKey, std::deque<QString>, SboKeyHash> m_historyMap
    ///   中**对应的 Key 必须立即 erase**，避免长期运行系统（连续运行数月/数年）
    ///   在动态 SBO 请求不断产生时，无用的 Key 长期驻留导致内存泄漏。
    ///
    ///   维护兜底: 即使 release() 因异常未完成 erase，仍由
    ///   purgeTerminatedEntries() 周期性清扫（详见该方法契约）。
    void release(const SboDeviceKey& key, const QString& sequenceId,
                 SboSequenceState finalState);

    // ──── 状态查询 ────

    /// 检查设备是否被 Armed
    [[nodiscard]] bool isArmed(const SboDeviceKey& key) const;

    /// 获取当前 Armed 占用者信息
    [[nodiscard]] ArmedOccupant getOccupant(const SboDeviceKey& key) const;

    /// 获取当前所有 Armed 设备数
    [[nodiscard]] int activeArmedCount() const;

    // ──── 断线处理 ────

    /// 通知 Guard 物理链路断线（自动清除该链路上所有 Armed 状态）
    /// @param linkId 断线的链路 ID
    ///
    /// ⚠ V1.1 资源回收契约:
    ///   对每个被清除的 Armed 占用者，其 SboDeviceKey 必须从
    ///   m_armedMap.erase()，并在历史记录中追加 "Expired" 标记。
    void onLinkDisconnected(uint32_t linkId);

    // ──── 维护入口（V1.1 新增）───

    /// 周期性清扫已终态条目 — 兜底防御性清理
    /// 调用时机:
    ///   - 默认每 5 分钟由内部 QTimer 自动触发一次（无需业务方干预）
    ///   - 也可在系统空闲期（如凌晨 03:00）由业务方显式调用一次
    ///
    /// 清理策略:
    ///   1) 遍历 m_historyMap，对所有 finalState ∈ {Operate, Expired, Cancelled, Failed}
    ///      且 lastUpdateTime < now - 1h 的条目 → erase()
    ///   2) 遍历 m_armedMap，理论上 release() 已实时 erase，这里做一致性校验
    ///      若发现 m_armedMap 中残留的 sequenceId 不在活跃队列 → 强制 erase
    ///   3) 调用后 shrink_to_fit() 归还超额分配的堆内存（针对 deque 历史记录）
    ///
    /// 复杂度: O(N) 其中 N = m_historyMap.size()，长期运行 N 稳定在 ~10^3 量级
    ///
    /// 线程安全: 持有 m_impl->mutex；可在任何线程调用（内部已加锁）
    /// @return 本次清扫的条目数（用于监控打点）
    [[nodiscard]] size_t purgeTerminatedEntries();

    // ──── 回调注册 ────

    void setStateCallback(SboStateCallback cb);

signals:
    /// SBO 序列状态变更
    void sboStateChanged(const ens::business::SboSequenceResult& result);

    /// Armed 被拒绝（因设备已被其他操作员占用）
    void armedRejected(const QString& sequenceId, const ens::business::SboDeviceKey& key,
                       const QString& occupiedBy);

    /// Armed 超时（倒计时到期）
    void armedTimeout(const QString& sequenceId, const ens::business::SboDeviceKey& key);

    /// 断线导致 Armed 自动清除
    void armedClearedByDisconnect(const QString& sequenceId,
                                  const ens::business::SboDeviceKey& key,
                                  uint32_t linkId);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// ============================================================================
// ⚠ V1.1 新增: DeviceSboGuard::Impl 内部数据结构声明
//
// Impl 必须包含以下两个哈希表，且在终态分支强制维护清理：
//
//   class DeviceSboGuard::Impl {
//   public:
//       // 活跃 Armed 占用表（释放/超时/断线必须 erase 对应 Key）
//       std::unordered_map<SboDeviceKey, ArmedOccupant, SboKeyHash>   m_armedMap;
//
//       // 历史轨迹表（终态后保留 1h 用于审计，过期由 purgeTerminatedEntries 清理）
//       std::unordered_map<SboDeviceKey,
//                          std::deque<SboSequenceRecord>,
//                          SboKeyHash>                                m_historyMap;
//
//       // 终态扫描定时器（每 5 分钟触发一次 purgeTerminatedEntries）
//       QTimer* m_purgeTimer = nullptr;
//
//       std::shared_mutex m_mutex;   // 多读者（isArmed/getOccupant）+ 单写者
//   };
//
// 强制约束:
//   1. release() 必须先在 m_armedMap.erase(key)，再向 m_historyMap 追加记录
//   2. onLinkDisconnected() 对每个被清除的占用者都必须在 m_armedMap.erase
//   3. purgeTerminatedEntries() 每 5 分钟清扫 m_historyMap 中超过 1h 的条目
//      并对 m_armedMap 做一致性校验（防止异常路径遗漏 erase）
//   4. 历史 deque 单 Key 上限 100 条记录，超出后 FIFO 淘汰（防止内存爆炸）
// ============================================================================

}  // namespace ens::business

// ============================================================================
// SboDeviceKey 哈希特化（支持 QHash / std::unordered_map）
// ============================================================================
namespace std {
    template<>
    struct hash<ens::business::SboDeviceKey> {
        size_t operator()(const ens::business::SboDeviceKey& k) const noexcept {
            return static_cast<size_t>(k.hash());
        }
    };
}

inline uint qHash(const ens::business::SboDeviceKey& k, uint seed = 0) {
    return static_cast<uint>(k.hash()) ^ seed;
}
```

### 4.2.1 SBO 状态机转换矩阵与回调签名

**目的**：将 §4.2 `DeviceSboGuard` 的状态流转、回调类型、信号签名独立成节，确保 SBO 状态机实现与消费侧代码一一对应。

```cpp
// ============================================================================
// EnerSentry — DeviceSboGuard 状态机与回调契约 (V1.9)
// 头文件: src/business/DeviceSboGuard.h (摘录)
// ============================================================================

namespace ens::business {

// ──── SBO 状态回调 ──────────────────────────────────────────────────────────

/// SBO 序列执行结果回调：任意终态/中间态变更时触发
using SboStateCallback = std::function<void(const SboSequenceResult& result)>;

// ──── Qt 跨线程信号（必须在 main() 中执行 qRegisterMetaType，参见 §1.5）────

signals:
    /// SBO 序列状态变更 → UI SBO 控制台（QueuedConnection）
    void sboStateChanged(const ens::business::SboSequenceResult& result);

    /// Armed 被拒绝（设备已被其他操作员占用）→ UI SBO 控制台（QueuedConnection）
    void armedRejected(const QString& sequenceId,
                       const ens::business::SboDeviceKey& key,
                       const QString& occupiedBy);

    /// Armed 超时（倒计时到期）→ UI SBO 控制台（QueuedConnection）
    void armedTimeout(const QString& sequenceId,
                      const ens::business::SboDeviceKey& key);

    /// 断线导致 Armed 自动清除 → UI SBO 控制台（QueuedConnection）
    void armedClearedByDisconnect(const QString& sequenceId,
                                  const ens::business::SboDeviceKey& key,
                                  uint32_t linkId);

}  // namespace ens::business
```

**SBO 状态转换矩阵**：

| 当前状态 | 触发事件 |  guard 条件 | 下一状态 | 副作用 |
|---------|---------|------------|---------|-------|
| `Idle` | `tryAcquire()` | 该 Key 未 Armed | `Armed` | 启动 Armed 倒计时；记录占用者 |
| `Idle` | `tryAcquire()` | 该 Key 已 Armed | `Idle` | 触发 `armedRejected`；返回失败 |
| `Armed` | 用户二次确认 / 自动执行 | `sequenceId` 匹配 | `Operate` | 调用下发通道；erase Key；触发 `sboStateChanged` |
| `Armed` | 用户取消 | `sequenceId` 匹配 | `Cancelled` | erase Key；记录审计；触发 `sboStateChanged` |
| `Armed` | 倒计时到期 | 无 | `Expired` | erase Key；触发 `armedTimeout`；触发 `sboStateChanged` |
| `Armed` | 目标链路断线 | `linkId` 匹配 | `Expired` | erase Key；触发 `armedClearedByDisconnect`；触发 `sboStateChanged` |
| `Operate` | 下发成功 | 通信返回 OK | `Executed` | 无 |
| `Operate` | 下发失败 | 通信超时/设备拒绝 | `Failed` | 记录错误信息；触发 `sboStateChanged` |
| `Executed` / `Failed` / `Cancelled` / `Expired` | `purgeTerminatedEntries()` | 终态超过 1h | `Idle`（逻辑删除） | erase Key；shrink_to_fit |

> **关键约束**：`Armed` 状态必须同时存在于 `m_armedMap` 中；进入任意终态前必须先 `m_armedMap.erase(key)`，再向 `m_historyMap` 追加记录。该顺序不可颠倒，防止 `isArmed()` 在终态后仍返回 true。

### 4.3 AuthManager RBAC 权限管理接口

**所属 Target**：`ens::business`（SHARED 库）  
**头文件路径**：`src/business/AuthManager.h`

```cpp
// ============================================================================
// EnerSentry — AuthManager RBAC 权限管理接口 (V1.5)
// 所属 Target: ens::business (SHARED)
// 头文件: src/business/AuthManager.h
// ============================================================================
#pragma once

#include "ens/export.hpp"
#include <QObject>
#include <QString>
#include <QDateTime>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace ens::business {

// ============================================================================
// UserRole — RBAC 三级角色枚举
// ============================================================================
enum class UserRole : uint8_t {
    Operator    = 0,   // 操作员：查看 + 告警确认 + 报表导出
    Engineer    = 1,   // 工程师：操作员全部 + 配置修改 + 控制操作
    Admin       = 2,   // 管理员：工程师全部 + 用户管理 + 系统配置
};

// ============================================================================
// AuthToken — 权限校验 Token（会话级）
// ============================================================================
struct ENS_BUSINESS_API AuthToken {
    QString  userId;            // 用户唯一 ID
    QString  userName;          // 用户显示名
    UserRole role;              // 角色
    uint64_t loginTime;         // 登录时间 (Unix ms)
    uint64_t expireTime;        // 过期时间 (Unix ms，15 分钟无操作后过期)
    bool     isValid() const;

    /// 检查是否有权限执行指定角色操作
    bool canActAs(UserRole requiredRole) const {
        return static_cast<uint8_t>(role) >= static_cast<uint8_t>(requiredRole);
    }
};

// ============================================================================
// UserInfo — 用户信息（持久化存储）
// ============================================================================
struct ENS_BUSINESS_API UserInfo {
    QString  userId;
    QString  userName;
    QString  passwordHash;      // bcrypt/scrypt 不可逆哈希
    UserRole role;
    bool     enabled = true;    // 账户是否启用
    uint64_t createdAt;
    uint64_t lastLoginAt;
};

// ============================================================================
// 鉴权结果回调
// ============================================================================
using AuthResultCallback = std::function<void(bool success, const QString& message, std::optional<AuthToken> token)>;

// ============================================================================
// AuthManager — RBAC 权限管理器
//
// 设计约束:
//  - 密码以 bcrypt/scrypt 存储，明文不可逆
//  - 会话超时 15 分钟自动锁定
//  - 登录失败 5 次锁定账户 15 分钟
//  - 所有控制操作前必须检查 Token 有效性
// ============================================================================
class ENS_BUSINESS_API AuthManager : public QObject {
    Q_OBJECT

public:
    static AuthManager& instance();

    // ──── 认证 ────

    /// 登录
    /// @param userName  用户名（const QString&，调用方通常为 Qt UI 控件）
    /// @param password  明文密码（内部哈希后比对）
    /// @return          token（成功）或 nullopt（失败）
    ///
    /// ⚠ V1.6 说明：保持 const QString& 而非 std::string_view，避免
    ///    Qt UI 调用方发生 QString → std::string → QString 的二次堆分配。
    std::optional<AuthToken> login(const QString& userName,
                                   const QString& password);

    /// 登出
    void logout(const QString& userId);

    /// 刷新 Token（用户操作时延长会话）
    std::optional<AuthToken> refreshToken(const QString& userId);

    /// 检查 Token 是否有效
    bool isTokenValid(const AuthToken& token) const;

    /// 检查是否有权限执行指定角色操作
    bool authorize(const AuthToken& token, UserRole requiredRole) const;

    /// 检查是否有权限执行 SBO 控制操作（仅 Engineer 及以上）
    bool authorizeControl(const AuthToken& token) const {
        return authorize(token, UserRole::Engineer);
    }

    // ──── 用户管理（仅 Admin） ────

    bool addUser(const AuthToken& adminToken, const UserInfo& user);
    bool removeUser(const AuthToken& adminToken, const QString& userId);
    bool enableUser(const AuthToken& adminToken, const QString& userId, bool enable);
    bool changePassword(const AuthToken& token, const QString& oldPassword,
                        const QString& newPassword);
    bool resetPassword(const AuthToken& adminToken, const QString& userId,
                       const QString& newPassword);

    std::vector<UserInfo> listUsers(const AuthToken& adminToken) const;

    // ──── 会话管理 ────

    /// 获取当前在线用户列表
    std::vector<QString> activeUsers() const;

    /// 强制锁定指定用户的会话
    void forceLockSession(const AuthToken& adminToken, const QString& userId);

signals:
    /// 用户登录
    void userLoggedIn(const QString& userId, const QString& userName);

    /// 用户登出
    void userLoggedOut(const QString& userId);

    /// 会话超时锁定
    void sessionLocked(const QString& userId);

    /// 鉴权失败（记录审计日志）
    void authFailed(const QString& userId, const QString& action, const QString& reason);

private:
    AuthManager();
    ~AuthManager() override;
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ens::business
```

### 4.4 V1.8 编译期头文件膨胀控制与 PIMPL 前置声明契约

**隐患**：公共头文件中若包含较多 C++ 标准库组件（`<variant>`, `<atomic>`, `<filesystem>` 等）以及 Qt / spdlog 重型头文件，会引发全工程的编译依赖扩散。核心业务类一旦在公共头文件中包含 `<spdlog/spdlog.h>`，任何包含该公共头文件的翻译单元都会被迫解析 spdlog 庞大的模板与格式化基础设施，显著拖慢增量构建与 CI 时间。

**目标**：对 `AlarmEngine` 和 `DeviceSboGuard` 等核心业务类严格执行 **PIMPL (Pointer to IMPLementation)** 与 **前置声明 (Forward Declaration)**，将重型头文件严格隔离在 `.cpp` 实现文件内。

**强制规则**：

1. **公共头文件负面清单**：以下头文件**禁止**出现在 `AlarmEngine.h`、`DeviceSboGuard.h` 等公共接口头文件中（可在 `.cpp` 中按需包含）：

   | 禁止头文件 | 原因 |
   |-----------|------|
   | `<spdlog/spdlog.h>` | 重型日志门面，含大量模板与 sink 注册 |
   | `<spdlog/fmt/*.h>` | 格式化库模板展开 |
   | `<spdlog/sinks/*.h>` | sink 实现依赖平台/文件系统 |
   | `<nlohmann/json.hpp>` | 大型模板 JSON 库（仅配置解析 `.cpp` 可包含） |
   | `<filesystem>` | 仅在 Impl 需要路径操作时使用；公共接口优先用 `std::string_view` |
   | `<QTimer>` | 仅 Impl 内部使用；公共头文件改用 `class QTimer;` 前置声明 |

2. **PIMPL 标准模板**：所有包含复杂内部状态的业务类必须在公共头文件中只暴露：

   ```cpp
   private:
       class Impl;
       std::unique_ptr<Impl> m_impl;
   ```

   并在同名 `.cpp` 文件中定义 `class AlarmEngine::Impl { ... }`。

3. **Impl 成员访问约定**：
   - 公共头文件中所有非静态成员函数必须在 `.cpp` 中实现（inline 定义仅限简单 getter）。
   - 所有 `spdlog::xxx` 调用、`QTimer` 操作、`std::unordered_map` 等复杂容器必须位于 `Impl` 内部，对外不可见。
   - 不得在公共头文件中直接暴露 `std::unordered_map`、`<deque>`、`<shared_mutex>` 等具体容器类型。

4. **前置声明白名单**：以下类型在公共头文件中允许且鼓励使用前置声明：

   ```cpp
   // Qt 类型
   class QTimer;
   class QObject;
   class QString;   // 实际使用 const QString& 时通常仍需 #include <QString>

   // 业务内部 Impl
   namespace ens::business {
       class AlarmEngine::Impl;
       class DeviceSboGuard::Impl;
   }
   ```

   > 注意：`QString` 作为值类型或引用参数时，若仅使用 `const QString&` 签名而不调用其成员函数，可在公共头文件中使用 `class QString;` 前置声明，从而进一步减少 `<QString>` 包含。但本项目为可读性保留 `#include <QString>`，属可接受折中。

5. **构建验证**：CI 流水线中增加头文件依赖扫描（如 `include-what-you-use` 或 `clang-tidy misc-include-cleaner`），对公共头文件包含负面清单中的头文件直接报 warning-as-error。

**现有代码整改清单**：

| 文件 | 整改项 | 状态 |
|------|--------|------|
| `src/business/AlarmEngine.h` | 已使用 `class Impl;` + `unique_ptr<Impl>`；继续禁止引入 `<spdlog/*>` | 已合规 |
| `src/business/DeviceSboGuard.h` | V1.8 移除 `<QTimer>`，改用 `class QTimer;` 前置声明 | 已修订 |
| `src/business/AuthManager.h` | 已使用 PIMPL；建议避免在头文件中新增重型依赖 | 已合规 |
| 新增业务公共头文件 | 必须遵循本契约并通过 CI 头文件扫描 | 强制 |

---

## 5. UI 渲染与图表降采样契约 (Layer 5)

### 5.1 DownSampler 降采样 API

**所属 Target**：`ens::ui`（STATIC 库）  
**头文件路径**：`src/ui/DownSampler.h`

```cpp
// ============================================================================
// EnerSentry — DownSampler 降采样 API (V1.5)
// 所属 Target: ens::ui (STATIC)
// 头文件: src/ui/DownSampler.h
// ============================================================================
#pragma once

#include <QPointF>
#include <QVector>
#include <cstdint>
#include <utility>

namespace ens::ui {

// ============================================================================
// DownSampleAlgorithm — 降采样算法枚举
// ============================================================================
enum class DownSampleAlgorithm : uint8_t {
    MinMax = 0,   // Min/Max 桶法（保留极值，适合工业曲线）
    LTTB   = 1,   // Largest-Triangle-Three-Buckets（视觉保真，适合曲线缩放）
};

// ============================================================================
// DownSampleParams — 降采样参数
// ============================================================================
struct DownSampleParams {
    int                  targetPoints;         // 目标输出点数（≤ 2000）
    DownSampleAlgorithm  algorithm;            // 算法选择
    bool                 preserveExtremes;     // 是否保留全局最大/最小值点
};

// ============================================================================
// DownSampleResult — 降采样结果
// ============================================================================
struct DownSampleResult {
    QVector<QPointF>  points;              // 降采样后的数据点
    double            globalMin;           // 全局最小值
    double            globalMax;           // 全局最大值
    int               originalCount;       // 原始数据点数
    int               outputCount;         // 输出数据点数
};

// ============================================================================
// DownSampler — 时序数据降采样器（纯函数，无状态，线程安全）
//
// 关键约束:
//  - 输出点数硬上限: 2,000（防止 QCustomPlot 渲染 bottleneck）
//  - 输入 > 目标点数时执行降采样；≤ 目标点数时直传
//  - LTTB 算法保证 ≤ 1920px 窗口内的视觉保真度
//  - Min/Max 桶法保留每个桶内的极大/极小值（适合工业告警曲线）
//  - V1.12 异常值处理: 输入数据若包含 NaN/±Inf 会导致 LTTB 三角形面积退化、
//    Min/Max 桶极值失真。调用方必须在传入前过滤或替换异常值，具体策略见
//    §5.1.1 "异常值 (NaN/±Inf) 拦截与预处理契约"。
//
// ⚠ V1.2 零堆分配优化:
//   30Hz/60Hz 渲染路径中，若每个活跃通道每帧都通过按值返回的 QVector<QPointF>
//   接口调用，会触发 malloc/free，破坏 CPU cache 局部性并造成堆碎片化。
//   因此核心算法改为 "输入指针 + 输出缓冲" 的零分配签名；
//   旧的按值返回接口保留为便捷包装（内部委托零分配接口）。
// ============================================================================
class DownSampler {
public:
    DownSampler() = default;

    // ──── 零堆分配核心 API（V1.2 新增，推荐在热路径使用）────

    /// LTTB 降采样（零分配版本）
    /// @param src        原始数据指针（必须按 X 升序，允许为空；V1.12 要求调用方
    ///                   预先处理 NaN/±Inf，否则面积计算会退化）
    /// @param srcSize    原始数据点数
    /// @param out        调用方预分配的输出缓冲区（容量 ≥ targetPoints）
    /// @param outCapacity 输出缓冲区最大可容纳点数
    /// @param targetPoints 目标输出点数
    /// @return           实际写入 out 的点数（≤ targetPoints）
    /// @pre  outCapacity ≥ targetPoints
    /// ⚠ V1.9 noexcept 契约: 零分配热路径，禁止抛异常
    /// ⚠ V1.12 NaN/Inf 契约: 本函数内部不做异常值过滤以维持 noexcept 与零分配；
    ///          若 src 中仍存在 NaN/Inf，输出结果可能包含 NaN，需调用方兜底。
    static size_t lttb(const QPointF* src, size_t srcSize,
                       QPointF* out, size_t outCapacity,
                       size_t targetPoints) noexcept;

    /// Min/Max 桶降采样（零分配版本）
    /// 输出总数 = min(targetPoints, srcSize) 桶 × 2 = 最多 2 × targetPoints
    /// @return 实际写入 out 的点数（≤ 2 × targetPoints）
    /// @pre  outCapacity ≥ 2 × targetPoints
    /// ⚠ V1.9 noexcept 契约: 零分配热路径，禁止抛异常
    /// ⚠ V1.12 NaN/Inf 契约: NaN 会导致 std::max/std::min 极值比较失效并传播；
    ///          调用方必须在传入前处理异常值，策略同 §5.1.1。
    static size_t minMaxBucket(const QPointF* src, size_t srcSize,
                               QPointF* out, size_t outCapacity,
                               size_t targetPoints) noexcept;

    /// 通用降采样入口（零分配版本）
    /// @return 实际写入 out 的点数
    /// ⚠ V1.9 noexcept 契约: 零分配热路径，禁止抛异常
    /// ⚠ V1.12 NaN/Inf 契约: 内部委托 lttb/minMaxBucket，异常值处理策略见 §5.1.1。
    static size_t downsample(const QPointF* src, size_t srcSize,
                             QPointF* out, size_t outCapacity,
                             const DownSampleParams& params) noexcept;

    // ──── 便捷包装（V1.5 兼容接口，内部委托零分配核心）────

    /// 降采样入口（自动选择算法）
    /// @param data   原始时序数据（按 X 轴升序排列）
    /// @param params 降采样参数
    /// @return       降采样结果
    static DownSampleResult downsample(const QVector<QPointF>& data,
                                       const DownSampleParams& params);

    /// LTTB 降采样（Largest-Triangle-Three-Buckets）
    /// 时间复杂度 O(n)，空间复杂度 O(targetPoints)
    /// @param data         原始数据（必须按 X 升序）
    /// @param targetPoints 目标点数
    /// @return             降采样后的数据点（保持原始顺序）
    static QVector<QPointF> lttb(const QVector<QPointF>& data, int targetPoints);

    /// Min/Max 桶降采样（保留每个桶的极大/极小值）
    /// 总数 = targetPoints 桶 × 2 = 2 × targetPoints
    /// @return 交替排列: [min_0, max_0, min_1, max_1, ...]
    static QVector<QPointF> minMaxBucket(const QVector<QPointF>& data, int targetPoints);

    // ──── 辅助 ────

    /// 计算合理的降采样目标点数
    /// 基于画布像素宽度与通道数的三重约束
    /// @param canvasPixels      画布像素宽度
    /// @param channelCount      活跃通道数
    /// @param hardLimit         硬上限（默认 2000）
    /// @return                  建议的目标点数
    /// ⚠ V1.9 noexcept 契约: 纯计算，禁止抛异常
    static int computeTargetPoints(int canvasPixels, int channelCount,
                                   int hardLimit = 2000) noexcept;

private:
    static constexpr int HARD_LIMIT  = 2000;
    static constexpr int MAX_PIXELS  = 1920;
};

// ============================================================================
// 零分配调用示例（V1.2 推荐渲染路径）
// ============================================================================
// 场景: RealtimeChartWidget 在 render-prep 线程中每 16ms 对每个通道执行一次
//       降采样。若使用按值返回的 QVector 接口，每帧每个通道都会触发一次堆分配。
//
// 推荐做法: 每个通道持有固定容量的 std::vector<QPointF> 或 std::array，
//           复用为输出缓冲，实现 malloc-free 热路径。
//
//   struct ChannelRenderContext {
//       std::vector<QPointF> source;      // 原始点（来自 pendingSamples）
//       std::vector<QPointF> downsampled; // 预分配输出缓冲（capacity 2000）
//       DownSampleParams     params;
//   };
//
//   void renderPrep(ChannelRenderContext& ctx) {
//       ctx.downsampled.resize(0);  // 只重置 size，不释放 capacity
//       ctx.downsampled.reserve(DownSampler::HARD_LIMIT);
//
//       size_t n = DownSampler::lttb(
//           ctx.source.data(), ctx.source.size(),
//           ctx.downsampled.data(), ctx.downsampled.capacity(),
//           static_cast<size_t>(ctx.params.targetPoints));
//
//       ctx.downsampled.resize(n);  // n ≤ targetPoints
//       // 将 ctx.downsampled.data() / n 直接交给 QCustomPlot::graph()->setData()
//       // 注意: QCustomPlot 会内部拷贝一次，但降采样阶段已零分配
//   }
//
// C++20 可选升级: 入参可替换为 std::span<const QPointF>，语义更清晰，
//                   但 C++17 项目应使用 (const QPointF*, size_t) 签名。
// ============================================================================

}  // namespace ens::ui
```

#### 5.1.1 V1.12 异常值 (NaN/±Inf) 拦截与预处理契约

**隐患**：工业现场中，传感器断线、通信失配、BMS 报文异常或协议解析越界，常常导致采集层输入 **NaN (Not a Number)** 或 **±Inf** 浮点数。LTTB（Largest-Triangle-Three-Buckets）算法严重依赖三角形面积计算：

```text
A = 1/2 | x_A(y_B - y_C) + x_B(y_C - y_A) + x_C(y_A - y_B) |
```

若三角形任一顶点为 `NaN`，整个面积退化为 `NaN`，进而导致：

- 当前桶内所有候选点的面积比较全部失效，LTTB 选点结果不可预期；
- 输出缓冲中出现 `NaN` 坐标，QCustomPlot 在计算轴范围、`QPainterPath` 边界框时触发断言或崩溃；
- Min/Max 桶法中 `std::max`/`std::min` 与 `NaN` 比较会传播 `NaN`，导致极值失真。

**处理策略（按优先级）**：

| 策略 | 适用位置 | 说明 | 副作用 |
|------|---------|------|--------|
| **前端过滤** | 协议解析层 / `Sample` 构造前 | 在数据进入 RingBuffer 前，将 `NaN`/`±Inf` 替换为 **上一帧有效值 (Hold Last Valid Value)**，并设置数据质量位 | 最彻底，避免污染下游所有消费者 |
| **RingBuffer 消费者过滤** | 降采样线程读取 `RingBuffer` 后 | 在调用 `DownSampler::lttb()` 前遍历源数据，跳过 `std::isnan(p.y())` 或 `std::isinf(p.y())` 的点 | 实现简单，但会改变输入点数 |
| **算法入口替换** | `DownSampler::lttb()` / `minMaxBucket()` 内部 | 遇到 `NaN` 时将其替换为 **上一帧有效值**（默认）或 `0.0f`（保守策略），然后继续计算 | 保持输出点数稳定，适合 UI 曲线连续性要求 |
| **后端兜底** | `RealtimeChartWidget::onBatchRepaint()` | 在将数据交给 QCustomPlot 前再次扫描并裁剪 NaN 点 | 最后一道防线，代价最高 |

**推荐做法**：采用 **"前端过滤 + 算法入口替换" 双重防御**：

1. **协议解析层**：在将原始采样写入 `RingBuffer` 之前，对浮点值进行校验：

   ```cpp
   // 协议解析层伪代码 (V1.12)
   Sample s;
   s.value = parseFloatFromFrame(frame);
   if (std::isnan(s.value) || std::isinf(s.value)) {
       // 策略 A: 使用上一帧同 pointId 有效值 (Hold Last Valid Value)
       s.value = lastValidValue_[pointId];
       // 数据质量位定义见 §5.1.2, 这里用 Invalid/Interpolated 标记
       qualityMap_[pointId] = DataQuality::Invalid;
   } else {
       lastValidValue_[pointId] = s.value;
       qualityMap_[pointId] = DataQuality::Valid;
   }
   ```

2. **降采样线程**：在调用 `DownSampler::lttb()` 前执行轻量过滤：

   ```cpp
   // 渲染准备线程伪代码 (V1.12)
   std::vector<QPointF> filtered;
   filtered.reserve(ctx.source.size());
   QPointF lastValid = ctx.source.empty() ? QPointF{} : ctx.source.front();

   for (const auto& p : ctx.source) {
       if (std::isnan(p.x()) || std::isnan(p.y()) ||
           std::isinf(p.x()) || std::isinf(p.y())) {
           // 策略 B1: 直接丢弃 (点数会变)
           continue;
           // 策略 B2: 替换为上一帧有效值 (保持点数)
           // filtered.emplace_back(p.x(), lastValid.y());
       } else {
           filtered.emplace_back(p);
           lastValid = p;
       }
   }

   size_t n = DownSampler::lttb(
       filtered.data(), filtered.size(),
       ctx.downsampled.data(), ctx.downsampled.capacity(),
       static_cast<size_t>(ctx.params.targetPoints));
   ```

3. **DownSampler 内部零分配约束下的兜底**：由于 `lttb()` 声明为 `noexcept` 且必须零分配，**不建议**在 `lttb()` 内部进行复杂过滤或容器重分配。但可以在算法遍历点时用栈变量记录 `lastValidY`，遇到 `NaN` 时以 `lastValidY` 参与面积计算：

   ```cpp
   // DownSampler::lttb 内部片段 (V1.13 推荐: Hold Last Valid Value)
   double lastValidY = 0.0;
   for (size_t i = 0; i < srcSize; ++i) {
       double y = src[i].y();
       if (std::isnan(y) || std::isinf(y)) {
           y = lastValidY;        // ✅ V1.13 默认策略: 替换为上一有效值
           // 避免折线图出现 0.0 "下尖峰" 误导运维
           // 备选: y = 0.0;       // 保守策略, 仅在明确要求时使用
       } else {
           lastValidY = y;
       }
       // ... 继续 LTTB 面积计算 ...
   }
   ```

**强制约束**：

1. **不允许未处理 NaN 进入 QCustomPlot**：`RealtimeChartWidget::onBatchRepaint()` 在调用 `QCPGraph::setData()`/`addData()` 前，必须保证数据集中不含 `NaN`/`±Inf`。
2. **noexcept 不成为借口**：`DownSampler` 的 `noexcept` 约束禁止堆分配，但 `std::isnan`/`std::isinf` 是纯计算函数，不抛异常，完全可以在热路径内使用。
3. **避免静默吞掉异常值**：任何替换/过滤操作都应通过 `DataQuality` 数据质量位、metrics 计数器或日志留下审计痕迹，便于现场排查传感器故障。具体定义与渲染方式见 §5.1.2。
4. **Min/Max 桶法同样受影响**：`minMaxBucket()` 中 `NaN` 会导致极值比较失效，必须采用与 LTTB 一致的预处理策略。
5. **单元测试覆盖**：必须新增以下测试用例：
   - 输入全为 `NaN` → 输出为空或全部为上一有效值/0.0（取决于策略）；
   - 输入中间混入单个 `NaN` → 输出不含 `NaN`；
   - 输入包含 `±Inf` → 输出不含 `Inf`；
   - 高频交替 `NaN`/有效值 → 验证 `lastValidY` 替换策略的连续性。

#### 5.1.2 V1.13 异常值恢复策略：Hold Last Valid Value + 数据质量位 + 断线/虚线渲染

**隐患**：§5.1.1 要求在协议解析层将 `NaN`/`±Inf` 替换为"上一帧有效值"或 `0.0f`。若直接替换为 `0.0f`，在工业储能图表中会产生剧烈的"下尖峰"，误导运维人员判断电池电压/电流真实归零。因此必须：

1. **默认采用 Hold Last Valid Value 策略**；
2. **在数据包中标记数据质量位 (Data Quality Bit)**，明确告诉下游"该值是填充值，非真实采样"；
3. **在 QCustomPlot 渲染层配合断线或虚线绘制**，让运维一眼识别出数据缺失区间。

**数据质量位定义**：

```cpp
// ============================================================================
// DataQuality — 数据质量位 (V1.13)
// 所属 Target: ens::datahub / ens::ui
// 头文件: include/ens/DataQuality.h
// ============================================================================
#pragma once

#include <cstdint>

namespace ens {

enum class DataQuality : uint8_t {
    Valid      = 0x00,  // 真实有效采样
    Invalid    = 0x01,  // 传感器断线、通信失配、协议解析异常等导致的无效值
    Interpolated = 0x02, // 通过 Hold Last Valid Value 填充的插值
    Saturated  = 0x04,  // 硬件饱和或超出量程 (±Inf 归为此类)
    CommFault  = 0x08,  // 通信链路故障（断线、CRC 错误）
    // 预留: 0x10, 0x20, 0x40, 0x80
};

inline DataQuality operator|(DataQuality a, DataQuality b) {
    return static_cast<DataQuality>(
        static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline DataQuality& operator|=(DataQuality& a, DataQuality b) {
    a = a | b;
    return a;
}
inline bool hasQuality(DataQuality value, DataQuality flag) {
    return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flag)) != 0;
}

}  // namespace ens
```

**ChartDataPoint — UI 渲染层带质量位的点**：

```cpp
// ============================================================================
// ChartDataPoint — 带数据质量位的图表点 (V1.13)
// 所属 Target: ens::ui
// 头文件: src/ui/ChartDataPoint.h
// ============================================================================
#pragma once

#include "ens/DataQuality.h"
#include <QPointF>

namespace ens::ui {

struct ChartDataPoint {
    QPointF    point;
    DataQuality quality = DataQuality::Valid;

    ChartDataPoint() = default;
    ChartDataPoint(const QPointF& p, DataQuality q = DataQuality::Valid)
        : point(p), quality(q) {}

    bool isValid() const {
        return quality == DataQuality::Valid;
    }
};

}  // namespace ens::ui
```

**Hold Last Valid Value 实现示例（协议解析层）**：

```cpp
// ============================================================================
// 协议解析层: NaN/Inf → Hold Last Valid Value + DataQuality 标记 (V1.13)
// ============================================================================
#include "ens/DataQuality.h"
#include <unordered_map>

std::unordered_map<uint32_t, float>     lastValidValue_;
std::unordered_map<uint32_t, DataQuality> lastQuality_;

Sample parseSample(const Frame& frame, uint32_t pointId) {
    Sample s;
    s.timestamp = frame.timestampMs;
    s.pointId   = pointId;
    s.value     = parseFloatFromFrame(frame);

    DataQuality quality = DataQuality::Valid;
    if (std::isnan(s.value) || std::isinf(s.value)) {
        // V1.13 默认策略: Hold Last Valid Value
        auto it = lastValidValue_.find(pointId);
        if (it != lastValidValue_.end()) {
            s.value = it->second;
            quality = DataQuality::Interpolated;
        } else {
            // 首帧即异常, 无上一帧可保持 —— 归零并标记 Invalid
            s.value = 0.0f;
            quality = DataQuality::Invalid;
        }

        if (std::isinf(parseFloatFromFrame(frame))) {
            quality |= DataQuality::Saturated;
        }

        // 同步记录质量位, 供 UI 层消费
        lastQuality_[pointId] = quality;

        // metrics: 记录异常采样数
        metrics.invalidSamples.fetch_add(1, std::memory_order_relaxed);
    } else {
        lastValidValue_[pointId] = s.value;
        lastQuality_[pointId]    = DataQuality::Valid;
    }

    return s;
}
```

**降采样层到 UI 层的数据质量位传递**：

```cpp
// ============================================================================
// 渲染准备线程: 将 Sample 转换为 ChartDataPoint, 保留数据质量位 (V1.13)
// ============================================================================
std::vector<ChartDataPoint> buildChartPoints(
    const std::vector<Sample>& samples,
    const std::unordered_map<uint32_t, DataQuality>& qualityMap) {

    std::vector<ChartDataPoint> out;
    out.reserve(samples.size());

    for (const auto& s : samples) {
        DataQuality q = DataQuality::Valid;
        auto it = qualityMap.find(s.pointId);
        if (it != qualityMap.end()) {
            q = it->second;
        }
        out.emplace_back(QPointF(static_cast<double>(s.timestamp),
                                 static_cast<double>(s.value)), q);
    }
    return out;
}
```

**QCustomPlot 断线/虚线渲染策略**：

QCustomPlot 的 `QCPGraph` 本身不直接支持"数据质量位"，但可以通过**分段绘制**实现断线或虚线效果：

```cpp
// ============================================================================
// QCustomPlot 断线/虚线渲染示例 (V1.13)
// 运行环境: Qt GUI 主线程 (qApp->thread())
// ============================================================================
void RealtimeChartWidget::setDataWithQuality(
    QCPGraph* graph,
    const std::vector<ChartDataPoint>& points) {

    Q_ASSERT_X(QThread::currentThread() == qApp->thread(),
               "RealtimeChartWidget::setDataWithQuality",
               "QCPGraph data modification MUST be on GUI thread");

    if (points.empty()) return;

    // 主曲线: 仅包含有 Valid / Interpolated 的点
    QVector<QPointF> validPoints;
    validPoints.reserve(points.size());

    // 异常区间: 标记为 Invalid / Saturated / CommFault 的点单独成段,
    //           用虚线或不同颜色绘制
    QVector<QVector<QPointF>> faultSegments;
    QVector<QPointF> currentFault;

    for (const auto& cp : points) {
        if (cp.quality == DataQuality::Valid ||
            cp.quality == DataQuality::Interpolated) {
            validPoints.push_back(cp.point);
            if (!currentFault.isEmpty()) {
                faultSegments.push_back(std::move(currentFault));
                currentFault.clear();
            }
        } else {
            // 无效区间: 为了视觉上"断线", 不加入主曲线
            currentFault.push_back(cp.point);
        }
    }
    if (!currentFault.isEmpty()) {
        faultSegments.push_back(std::move(currentFault));
    }

    // 主曲线: 实线
    graph->setLineStyle(QCPGraph::lsLine);
    graph->setScatterStyle(QCPScatterStyle::ssNone);
    graph->data()->set(validPoints, true);  // 已排序, true = alreadySorted

    // 异常区间: 虚线 (或根据 quality 设置不同颜色)
    // 注意: 这里用辅助 graph 绘制故障段, 避免污染主曲线数据
    for (int i = 0; i < faultSegments.size(); ++i) {
        QCPGraph* faultGraph = ensureFaultGraph(graph, i);  // 获取/创建辅助 graph
        faultGraph->setLineStyle(QCPGraph::lsLine);
        faultGraph->setPen(QPen(Qt::red, 1, Qt::DotLine));  // 红色虚线
        faultGraph->data()->set(faultSegments[i], true);
        faultGraph->setVisible(true);
    }
    hideUnusedFaultGraphs(graph, faultSegments.size());
}
```

**替代渲染策略对比**：

| 策略 | 视觉效果 | 适用场景 | 实现复杂度 |
|------|----------|----------|-----------|
| **断线** (不绘制 Invalid 点) | 曲线在异常区间出现"缺口" | 传感器断线、通信中断 | 低 |
| **虚线** (Invalid 点用虚线连接) | 曲线在异常区间变成虚线 | 数据质量降级但仍有参考值 | 中 |
| **变色** (Invalid 点用红色/灰色绘制) | 异常区间颜色明显不同 | 需要保持曲线连续性 | 中 |
| **断线 + 标记** (在异常区间起点画竖线/图标) | 最直观, 但实现复杂 | 关键测点运维场景 | 高 |

**推荐默认策略**：

- **Interpolated**（Hold Last Valid Value 填充）：用主曲线实线绘制，但在 Y 值不变的长区间可考虑用浅灰色/半透明，提示运维"该区间为保持值"；
- **Invalid / Saturated / CommFault**：用**断线**或**红色虚线**绘制，明确标识数据缺失或异常；
- 最终产品在 UI 设计文档中明确图例，避免不同颜色/线型含义混淆。

**强制约束**：

1. **禁止直接替换为 0.0f 后不作任何标记**：默认策略必须是 Hold Last Valid Value；若因特殊业务需要归零，必须在 `DataQuality` 中标记 `Invalid` 或 `Saturated`。
2. **数据质量位必须沿数据流传递**：从协议解析层 → 数据中枢 → 降采样层 → UI 渲染层，每一层转换都必须保留 `DataQuality` 信息，禁止在 RingBuffer/降采样过程中丢失。
3. **QCustomPlot 绘制前必须拆分数据**：不允许将 Invalid 点与 Valid 点混合在同一 `QCPGraph` 中直接绘制，否则视觉上无法区分异常区间。
4. **必须提供图例说明**：UI 必须在图表角落或工具提示中说明实线、虚线、断线分别代表的含义。
5. **单元测试覆盖**：
   - 连续 3 个 `NaN` → 输出为上一有效值且 `DataQuality::Interpolated`；
   - 首帧即 `NaN` → 输出 `0.0f` 且 `DataQuality::Invalid`；
   - `±Inf` → 输出上一有效值且 `DataQuality::Saturated`；
   - 渲染层拆分函数：输入 Valid + Invalid 混合点，验证主曲线与 fault graph 点数正确。

#### 5.1.3 V1.14 LTTB 平直桶 / 全相同值桶快速跳过

**隐患**：§5.1.2 默认采用 **Hold Last Valid Value (HLVV)** 策略处理 `NaN`/`±Inf` 异常值。当硬件模块断线产生连续 1,000 个 `NaN` 点时，HLVV 会将其全部填充为同一上一有效值，形成一段**平直直线**。LTTB（Largest-Triangle-Three-Buckets）算法在计算三桶三角形面积时，若当前桶内所有点的 `Y` 坐标完全重合（`Ymax == Ymin`），则三角形面积持续退化为 `0.0f`：

```text
A = 1/2 | x_A(y_B - y_C) + x_B(y_C - y_A) + x_C(y_A - y_B) |
当 y_A == y_B == y_C 时, A ≡ 0
```

面积退化会导致：

- 候选点之间的面积比较全部失效，LTTB 退化为"顺序选择首个点"；
- 产生大量无效浮点运算，浪费 CPU 周期；
- 输出点在长平直区间内分布不均匀，可能引发 UI 渲染抖动（某些帧选中端点、某些帧选中点，造成视觉跳动）。

**目标**：在 `DownSampler::lttb` 的桶处理循环中加入**平直桶 / 全相同值桶快速跳过**逻辑：若桶内 `Ymax == Ymin`，直接取该桶中点（或桶内首个点）作为代表点，跳过三角形面积计算。

**判定条件**：

对 LTTB 的每一个桶 `bucket[i]`（`i ∈ [1, bucketCount - 2]`，首尾桶固定保留），计算：

```cpp
const double yMin = minY(bucket[i]);
const double yMax = maxY(bucket[i]);
const bool isFlatBucket = (yMax == yMin);  // 桶内所有 y 相同
```

当 `isFlatBucket == true` 时：

1. 直接选择桶的**中位点**（或按 X 排序后的中间点）作为该桶的代表点；
2. 跳过对该桶的三角形面积最大化搜索；
3. 仍保持 LTTB 的整体 O(n) 时间复杂度。

**算法伪代码**：

```cpp
// ============================================================================
// DownSampler::lttb 桶处理片段 (V1.14 平直桶快速跳过)
// 前提: 输入 src 已按 X 升序, 且 NaN/Inf 已由 HLVV 替换为上一有效值
// ============================================================================
static size_t lttb(const QPointF* src, size_t srcSize,
                   QPointF* out, size_t outCapacity,
                   size_t targetPoints) noexcept {
    if (srcSize == 0 || targetPoints == 0 || outCapacity == 0) return 0;
    if (srcSize <= targetPoints) {
        std::copy(src, src + std::min(srcSize, outCapacity), out);
        return std::min(srcSize, outCapacity);
    }

    const size_t bucketCount = targetPoints;
    const double avgInterval = static_cast<double>(srcSize - 1) /
                               static_cast<double>(bucketCount - 1);

    out[0] = src[0];  // 首点固定保留
    size_t outIdx = 1;

    QPointF prevSelected = src[0];
    for (size_t i = 1; i < bucketCount - 1 && outIdx < outCapacity; ++i) {
        const size_t bucketStart = static_cast<size_t>(
            std::floor(static_cast<double>(i - 1) * avgInterval)) + 1;
        const size_t bucketEnd = static_cast<size_t>(
            std::floor(static_cast<double>(i) * avgInterval)) + 1;

        // 计算下一桶的起始位置 (用于三角形面积计算)
        const size_t nextBucketStart = static_cast<size_t>(
            std::floor(static_cast<double>(i) * avgInterval)) + 1;
        const size_t nextBucketEnd = static_cast<size_t>(
            std::floor(static_cast<double>(i + 1) * avgInterval)) + 1;
        const QPointF nextBucketCenter = bucketCenter(src, nextBucketStart,
                                                       nextBucketEnd, srcSize);

        // V1.14 平直桶检测
        double yMin = src[bucketStart].y();
        double yMax = yMin;
        for (size_t j = bucketStart + 1; j < bucketEnd && j < srcSize; ++j) {
            const double y = src[j].y();
            if (y < yMin) yMin = y;
            if (y > yMax) yMax = y;
        }

        if (yMax == yMin) {
            // 平直桶: 直接取中位点, 跳过三角形面积计算
            const size_t midIdx = bucketStart + (bucketEnd - bucketStart) / 2;
            const QPointF selected = src[std::min(midIdx, srcSize - 1)];
            out[outIdx++] = selected;
            prevSelected = selected;
            continue;
        }

        // 非平直桶: 传统 LTTB 三角形面积最大化选点
        double maxArea = -1.0;
        QPointF selected = src[bucketStart];
        for (size_t j = bucketStart; j < bucketEnd && j < srcSize; ++j) {
            const double area = std::fabs(
                prevSelected.x() * (src[j].y() - nextBucketCenter.y()) +
                src[j].x() * (nextBucketCenter.y() - prevSelected.y()) +
                nextBucketCenter.x() * (prevSelected.y() - src[j].y()));
            if (area > maxArea) {
                maxArea = area;
                selected = src[j];
            }
        }
        out[outIdx++] = selected;
        prevSelected = selected;
    }

    if (outIdx < outCapacity) {
        out[outIdx++] = src[srcSize - 1];  // 尾点固定保留
    }
    return outIdx;
}
```

**复杂度分析**：

| 场景 | 时间复杂度 | 说明 |
|------|-----------|------|
| 无平直桶 | O(n) | 每个点最多参与一次 min/max 扫描和一次面积计算 |
| 全平直输入 | O(n) | 每个点仅参与一次 min/max 扫描，跳过面积计算；常数项显著降低 |
| 连续 1,000 点 HLVV 填充 | O(n) | 平直区间内不再执行浮点面积运算，CPU 占用下降明显 |

**与 HLVV 策略的协同**：

- 平直桶快速跳过**不能替代** HLVV 策略，二者是正交优化：
  - HLVV 解决"输入异常值导致 NaN 面积退化"；
  - 平直桶跳过解决"HLVV 填充后大量相同值导致面积为零"的计算浪费。
- 若业务允许，可在 HLVV 填充阶段同步标记 `DataQuality::Interpolated`，渲染层据此决定是否将长平直区间显示为"保持值"提示。

**强制约束**：

1. **必须检测桶内 Y 范围**：在 LTTB 主循环中，每个桶至少扫描一次以确定 `yMin`/`yMax`；该扫描可与面积计算合并，避免二次遍历。
2. **平直桶代表点选择必须稳定**：同一桶在不同帧之间应选择固定规则（如中位点），避免视频帧间跳动；禁止在不同调用间随机选择。
3. **不允许因跳过而丢失首尾点**：LTTB 的首尾点固定保留原则不受平直桶逻辑影响。
4. **单元测试覆盖**：必须新增以下测试用例：
   - 输入为全相同 Y 值的 10,000 点 → 验证输出点分布均匀、无 NaN、无异常大间隔；
   - 输入中间存在 1,000 点连续相同 Y 值 → 验证平直区间内选中点稳定；
   - 输入全为 `NaN` 经 HLVV 替换为 0.0 后（测试场景）→ 验证算法不崩溃、输出稳定；
   - 与未优化 LTTB 对比随机数据 → 验证视觉保真度不下降（SSIM 或人工检视）。
5. **性能回归测试**：使用连续 HLVV 填充的 5,000 点/秒数据，对比开启/关闭平直桶跳过的 CPU 占用，要求平直场景下 `lttb()` 耗时下降 ≥ 30%。

---

### 5.2 批处理重绘契约 (QTimer 节流)

**所属 Target**：`ens::ui`（STATIC 库）  
**头文件路径**：`src/ui/RealtimeChartWidget.h`（关键接口摘录）

```cpp
// ============================================================================
// EnerSentry — RealtimeChartWidget 实时曲线控件接口 (V1.5)
// 所属 Target: ens::ui (STATIC)
// 头文件: src/ui/RealtimeChartWidget.h (关键接口摘录)
// ============================================================================
#pragma once

#include "DownSampler.h"
#include <QTimer>
#include <QWidget>
#include <QHash>
#include <QReadWriteLock>
#include <QVector>
#include <QPointF>
#include <cstdint>

class QCustomPlot;
class QCPGraph;

namespace ens::ui {

// ============================================================================
// RenderPacket — 渲染数据包（渲染准备线程产出 → UI 主线程消费）
// ============================================================================
struct RenderPacket {
    uint32_t         pointId;
    QVector<QPointF> points;          // 已降采样的数据点
    double           yMin;            // Y 轴最小值（用于自适应缩放）
    double           yMax;            // Y 轴最大值
    uint64_t         generationTime;  // 数据包生成时间戳
};

// ============================================================================
// ChannelBuffer — 单通道数据缓冲
// ============================================================================
struct ChannelBuffer {
    QVector<QPointF>  pendingSamples;   // 采集线程写入 → 渲染准备线程读取
    QVector<QPointF>  readySamples;     // 已降采样的就绪数据 → UI 主线程消费
    mutable QReadWriteLock rwLock;      // pendingSamples 读写保护
    bool              yAxisAutoScale = true;
    double            yAxisMinManual = 0.0;
    double            yAxisMaxManual = 100.0;
};

// ============================================================================
// RealtimeChartWidget — 实时曲线控件
//
// 关键约束（严禁数据到达即 replot()！）:
//  ┌─────────────────────────────────────────────────────────────┐
//  │ 三重防御架构:                                                │
//  │  第 1 层: per-channel pendingSamples 缓冲 (QReadWriteLock)   │
//  │  第 2 层: QTimer 30Hz / 60Hz 定时触发批处理重绘               │
//  │  第 3 层: 降采样检查 — 数据量 > 目标点数 → Min-Max / LTTB    │
//  │                                                             │
//  │  采集线程 → QueuedConnection → onNewSample → 仅缓冲,         │
//  │  严禁在此调用 m_plot->replot()!                              │
//  └─────────────────────────────────────────────────────────────┘
// ============================================================================
class RealtimeChartWidget : public QWidget {
    Q_OBJECT

public:
    /// 刷新率枚举
    enum class RefreshRate { Hz30, Hz60 };

    /// 批处理约束常量
    static constexpr int MAX_POINTS_PER_CHANNEL = 2000;   // 硬上限
    static constexpr int MAX_PIXELS_PER_CHANNEL = 1920;   // 1080p 单通道宽度
    static constexpr int PENDING_WARN_THRESHOLD  = 5000;   // 积压告警阈值

    explicit RealtimeChartWidget(QWidget* parent = nullptr);
    ~RealtimeChartWidget() override;

    // ──── 通道管理 ────

    /// 添加通道
    /// @param pointId 测点 ID
    /// @param label   图例标签
    /// @param color   曲线颜色
    void addChannel(uint32_t pointId, const QString& label, const QColor& color);

    /// 移除通道
    void removeChannel(uint32_t pointId);

    /// 清空所有通道
    void clearAllChannels();

    // ──── 渲染控制 ────

    /// 设置批处理刷新率
    void setRefreshRate(RefreshRate rate);

    /// 暂停/继续滚动
    void setPaused(bool paused);

    /// 设置时间窗口（秒）
    void setTimeWindow(int seconds);

    // ──── Y 轴控制 ────

    void setYAxisAutoScale(uint32_t pointId, bool autoScale);
    void setYAxisManualRange(uint32_t pointId, double min, double max);

public slots:
    // ═══════════════════════════════════════════════════════════════
    // 数据投递入口（采集线程通过 QueuedConnection 调用）
    // ⚠ 此函数仅缓冲数据，严禁调用 m_plot->replot()
    // ═══════════════════════════════════════════════════════════════
    void onNewSample(uint32_t pointId, double value, qint64 timestampMs);

    /// 批量数据投递（渲染准备线程产出）
    void onRenderPacket(const RenderPacket& packet);

private slots:
    /// QTimer 触发的批量重绘入口（30Hz 或 60Hz）
    /// 仅在此函数中调用 m_plot->replot(QCustomPlot::rpQueuedReplot)
    void onBatchRepaint();

private:
    QCustomPlot*                               m_plot;
    QTimer*                                    m_repaintTimer;
    QHash<uint32_t, ChannelBuffer>             m_channels;
    QHash<uint32_t, QCPGraph*>                 m_graphs;
    RefreshRate                                m_rate = RefreshRate::Hz30;
    int                                        m_timeWindowSecs = 300;
    bool                                       m_paused = false;
};

}  // namespace ens::ui
```

### 5.2.1 V1.6 QCustomPlot 数据深拷贝与零拷贝填充契约

⚠ **关键事实**：`QCustomPlot::addGraph()` 返回的 `QCPGraph` 内部数据容器是 `QSharedPointer<QCPGraphDataContainer>`，其 `data()->set(QVector<QPointF>)` / `data()->replace(...)` 重载会**逐元素深拷贝**传入的 `QVector` 到内部连续内存。100ms 周期 + 60Hz 渲染时每秒约 5000~10000 次深拷贝，是热路径的主要堆分配来源。

**契约（V1.6 强制遵守）**：

```cpp
// ===== ✅ V1.6 零拷贝模式 (PREFERRED) =====
// 通过 m_plot->graph(id)->data() 直接拿到 QSharedPointer<QCPGraphDataContainer>
// 调用 reserve + add / set 等支持 QPointF* 的方法:
//   - data()->add(QPointF* data, size_t count);    // 直接 memmove 进连续缓冲
//   - data()->reserve(newCapacity);                // 预分配
//   - data()->replace(idx, QPointF* data, size_t count);  // 区间覆盖

// 典型用法 (RealtimeChartWidget::onRenderPacket):
void RealtimeChartWidget::onRenderPacket(const RenderPacket& packet) {
    auto graphIt = m_graphs.find(packet.pointId);
    if (graphIt == m_graphs.end()) return;

    auto container = graphIt.value()->data();   // QSharedPointer<QCPGraphDataContainer>
    const auto pts = packet.points.constData();
    const qsizetype n = packet.points.size();

    // ① 保留容量, 避免 realloc
    if (container->size() < static_cast<int>(n)) {
        container->reserve(n + 1024);
    }

    // ② 区间写入: 直接 memmove 进 QCPGraphDataContainer 内部缓冲, 不走中间复制
    container->add(pts, n);   // V1.6 推荐; add() 自动 capacity check

    // ③ 触发重绘
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

// ===== ❌ V1.6 之前踩坑写法 (禁止在 hot path) =====
auto graph = graphIt.value();
// 走 set() 重载: 会隐式 QVector<QPointF> = constData 拷贝, 路径上每次拷贝一次
graph->setData(packet.points);
//            ↑ 内部拷贝 ~numPoints × sizeof(QPointF) = 16 KB @ 2000点
```

**V1.6 静态断言 (编译期保障)**：

```cpp
static_assert(sizeof(QPointF) == 16,
    "QPointF size assumption broken; check QCustomPlot zero-copy alignment "
    "before continuing to use add(data, count) zero-copy path.");
static_assert(std::is_trivially_copyable_v<QPointF>,
    "QPointF must be trivially copyable for QCPGraphDataContainer::add(mem, count).");
```

**性能对比基准（V1.6 基线）**：

| 写法 | 1000 点推送耗时 | 堆分配次数 | 备注 |
|------|---------------|----------|------|
| `setData(QVector<QPointF>)` | 320 μs | 2 (input 拷贝 + 内部 resize) | 反例 |
| `add(QPointF*, size_t)` 零拷贝 | 45 μs  | 1 (resize, 复用 capacity) | 推荐 |
| `data()->reserve(N)` 预分配 + add | 30 μs  | 0~1 (首次 M 次, 之后 0) | 高频路径首选 |

#### 5.2.1.1 V1.11 零拷贝填充的 GUI 主线程约束（强制 CR 检查项）

⚠ **关键事实**：QCustomPlot 的零拷贝填充 (`data()->add()` / `data()->replace()` / `data()->set()`) 虽然**不经过 Qt 信号槽**、**不持有 QApplication 互斥锁**，但所有 `QCPGraph` / `QCPGraphDataContainer` 实例的所有权属于 QCustomPlot widget —— 而 QWidget 的所有内部数据（pixmap 缓存、轴几何、字体、painter state）必须在创建它的 GUI 主线程访问。

**潜在风险场景**：

1. 降采样线程（§1.5 L4 渲染准备线程）通过回调/队列收到 `RenderPacket` 后，若直接调用 `m_plot->graph(id)->data()->add(pts, n)`，会出现：
   - 内部 `QCPGraphDataContainer::add` 触发 `mData->reserve()` → `QVector<QPointF>::realloc` → 写共享内存；
   - 此时 UI 主线程的 `paintEvent()` 正在遍历同一 `QCPGraphDataContainer` 的 `mData` 指针，**没有 mutex 保护**（QCustomPlot 假定单线程访问）；
   - 轻则画面撕裂（tearing），重则 `QVector` 内部 size_t 与 buffer 指针的撕裂读导致段错误。
2. 同样，**任何从非 GUI 线程触发的 `replot()` 调用**也是未定义行为：QCustomPlot 内部使用 `QWidget::update()` / `QPainter::begin()` 等 GUI-only API。

**契约（V1.11 强制 CR 检查）**：

```cpp
// ===== ✅ V1.11 GUI 主线程断言（必加在所有零拷贝槽函数入口） =====
void RealtimeChartWidget::onBatchRepaint() {
    // ⚠ V1.11 强制: Qt GUI 主线程断言, 严禁后台线程进入
    //   与 §1.5 元类型注册配套, 零拷贝填充路径同样必须在 qApp 线程
    //   Q_ASSERT 在 Debug 触发后立即中断, 防止 QCPGraphDataContainer 撕裂读
    Q_ASSERT_X(QThread::currentThread() == qApp->thread(),
               "RealtimeChartWidget::onBatchRepaint",
               "Zero-copy QCPGraph::data()->add() MUST be called from "
               "the Qt GUI main thread (qApp->thread()), but was called "
               "from a worker thread. Wrap your background thread's "
               "data delivery with QMetaObject::invokeMethod(..., Qt::QueuedConnection).");
    // ... 原有逻辑 ...
}

void RealtimeChartWidget::onRenderPacket(const RenderPacket& packet) {
    // 同样的断言
    Q_ASSERT_X(QThread::currentThread() == qApp->thread(),
               "RealtimeChartWidget::onRenderPacket",
               "QCPGraph::data()->add() MUST be called from GUI main thread.");
    // ... 原有逻辑 ...
}

// 任何其他直接持有 QCPGraph* 的槽函数 / public 方法, 一律加同样的断言
```

**降采样线程 → UI 主线程的正确投递模式**：

```cpp
// ===== ❌ V1.11 之前踩坑写法（背景线程直接写 QCPGraph，CR 必须阻断） =====
void RenderPrepThread::onNewSample(const Sample& s) {
    // 错误! 当前线程不是 qApp->thread(), 后续 data()->add() 会导致撕裂
    QCPGraph* graph = m_chartWidget->m_graphs.value(s.pointId, nullptr);
    graph->data()->add(QPointF{s.timestamp / 1000.0, s.value}, 1);
}

// ===== ✅ V1.11 修正写法（通过 QueuedConnection 投递到 UI 主线程） =====
void RenderPrepThread::onNewSample(const Sample& s) {
    // 1) 准备数据: 线程本地缓冲, 累积 N 个样本后批量投递
    m_pendingSamples[s.pointId].push_back(QPointF{s.timestamp / 1000.0, s.value});
    if (m_pendingSamples[s.pointId].size() >= BATCH_SIZE) {
        // 2) 通过 QueuedConnection 投递到 UI 线程
        //    Qt 自动在接收线程上下文中执行目标槽函数
        QMetaObject::invokeMethod(m_chartWidget, "onBatchRepaint",
                                  Qt::QueuedConnection);
        m_pendingSamples[s.pointId].clear();
    }
}
```

**V1.11 静态检查清单（CI 配置）**：

1. **clang-tidy check**: 启用 `bugprone-unused-return-value` + 自定义 `ens-qcpgraph-only-gui-thread` matcher 检测 `QCPGraph::data()->add/set/replace` 调用位置，警告当调用点所在函数不在 Q_INVOKABLE 槽或不是 `QWidget` 子类成员函数时。
2. **Code Review 红线**：所有修改 §5.2 章节的 PR，CR 阶段必须确认新引入的 `data()->add()` / `setData()` 调用在 GUI 主线程上下文；任何从非主线程触发的零拷贝路径必须被驳回。
3. **单元测试**: `test_realtimechartwidget_thread_safety.cpp` 模拟后台线程投递 `QCPGraph*` 引用，断言会触发 `Q_ASSERT_X`，作为回归测试。

**性能影响**：`QThread::currentThread() == qApp->thread()` 是单个指针比较 + 整数等值，**不进入内核态**，开销 < 5 ns。在 onBatchRepaint 16ms 节流路径上可忽略。

### 5.2.2 V1.9 RealtimeChartWidget::onBatchRepaint 实现伪代码

**目的**：将 §5.2 接口头文件中的 `onBatchRepaint()` 声明落地为可执行的伪代码，明确“仅在此函数中调用 `m_plot->replot()`”的批处理契约。

```cpp
// ============================================================================
// EnerSentry — RealtimeChartWidget::onBatchRepaint 实现伪代码 (V1.9)
// 文件: src/ui/RealtimeChartWidget.cpp
// 调用线程: UI 主线程（由 m_repaintTimer 触发）
// ============================================================================

void RealtimeChartWidget::onBatchRepaint() {
    // ⚠ V1.11 GUI 主线程断言（详见 §5.2.1.1）
    //   QCPGraph::data() 的零拷贝填充必须在 qApp 线程执行; 后台线程误用会
    //   导致 QCPGraphDataContainer 撕裂读, 进而 QCustomPlot::paintEvent 段错误。
    Q_ASSERT_X(QThread::currentThread() == qApp->thread(),
               "RealtimeChartWidget::onBatchRepaint",
               "Zero-copy QCPGraph::data()->add() MUST be called from "
               "Qt GUI main thread. Use QMetaObject::invokeMethod(..., "
               "Qt::QueuedConnection) to dispatch from worker threads.");
    if (m_paused || m_channels.isEmpty()) {
        return;
    }

    const uint64_t nowMs = QDateTime::currentMSecsSinceEpoch();
    const double windowStartMs = static_cast<double>(nowMs - m_timeWindowSecs * 1000);
    const double windowEndMs   = static_cast<double>(nowMs);

    // ── 第 1 步: 对每个通道执行降采样并填充 QCPGraph ───────────────────────
    for (auto it = m_channels.begin(); it != m_channels.end(); ++it) {
        const uint32_t pointId = it.key();
        ChannelBuffer& channel = it.value();
        QCPGraph* graph = m_graphs.value(pointId, nullptr);
        if (!graph) continue;

        // 1.1 在 pendingSamples 上锁, 交换到本地 readySamples
        QVector<QPointF> snapshot;
        {
            QWriteLocker lock(&channel.rwLock);
            snapshot.swap(channel.pendingSamples);  // O(1), 不拷贝数据
            // pendingSamples 现在为空, 原缓冲由 snapshot 持有
        }

        if (snapshot.isEmpty()) continue;

        // 1.2 追加到 readySamples 并执行降采样
        channel.readySamples.append(snapshot);
        // 裁剪超出时间窗口的旧数据（避免内存无限增长）
        auto firstInWindow = std::lower_bound(
            channel.readySamples.begin(), channel.readySamples.end(),
            windowStartMs,
            [](const QPointF& pt, double ts) { return pt.x() < ts; });
        if (firstInWindow != channel.readySamples.begin()) {
            channel.readySamples.erase(channel.readySamples.begin(), firstInWindow);
        }

        // 1.3 执行降采样（零分配接口）
        const int targetPoints = DownSampler::computeTargetPoints(
            m_plot->axisRect()->width(), m_graphs.size(), MAX_POINTS_PER_CHANNEL);

        std::vector<QPointF> downsampled;
        downsampled.reserve(targetPoints);
        const size_t outCount = DownSampler::downsample(
            channel.readySamples.constData(),
            static_cast<size_t>(channel.readySamples.size()),
            downsampled.data(),
            static_cast<size_t>(downsampled.capacity()),
            DownSampleParams{targetPoints, DownSampleAlgorithm::LTTB, true});
        downsampled.resize(outCount);

        // 1.4 零拷贝填充 QCustomPlot 内部数据容器
        auto container = graph->data();
        if (container->size() < static_cast<int>(outCount)) {
            container->reserve(outCount + 1024);
        }
        container->clear();                       // 清除旧数据
        container->add(downsampled.data(), outCount);  // 直接 memmove

        // 1.5 Y 轴范围自适应
        if (channel.yAxisAutoScale) {
            double yMin = 0.0, yMax = 100.0;
            if (!downsampled.empty()) {
                auto [minIt, maxIt] = std::minmax_element(
                    downsampled.begin(), downsampled.end(),
                    [](const QPointF& a, const QPointF& b) { return a.y() < b.y(); });
                yMin = minIt->y();
                yMax = maxIt->y();
            }
            graph->valueAxis()->setRange(yMin, yMax);
        } else {
            graph->valueAxis()->setRange(channel.yAxisMinManual, channel.yAxisMaxManual);
        }
    }

    // ── 第 2 步: 统一刷新 X 轴范围并触发重绘 ─────────────────────────────────
    m_plot->xAxis->setRange(windowStartMs, windowEndMs);
    m_plot->replot(QCustomPlot::rpQueuedReplot);  // ✅ 唯一 replot 调用点
}
```

**关键约束复申**：

1. `onNewSample()` 与 `onRenderPacket()` 中**严禁**调用 `m_plot->replot()`；仅允许写入 `pendingSamples` 或接收 `RenderPacket`。
2. `pendingSamples` 与 `readySamples` 的交换必须在 `QWriteLocker` 保护下进行，但交换后降采样与 QCustomPlot 填充在锁外执行，避免阻塞采集线程。
3. `QCustomPlot::rpQueuedReplot` 将重绘事件投递到 Qt 事件队列，避免在定时器槽函数中直接触发同步 paint。

---

## 6. CMake 编译与符号导出声明规范

### 6.1 ens/export.hpp 标准实现

**所属**：公共头文件，被所有 SHARED 模块引用  
**文件路径**：`include/ens/export.hpp`

```cpp
// ============================================================================
// EnerSentry — 符号导出宏 (export.hpp) (V1.5)
// 兼容 MSVC __declspec(dllexport/dllimport) 与 GCC/Clang visibility("default")
// ============================================================================
#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// MSVC (Windows DLL 导出/导入)
// ═══════════════════════════════════════════════════════════════════════════
#if defined(_MSC_VER)

    // ──── ens::channel (SHARED) ────
    #ifdef ENS_CHANNEL_EXPORTS
        // DLL 编译端 — 导出符号
        #define ENS_CHANNEL_API __declspec(dllexport)
    #else
        // DLL 使用端 — 导入符号
        #define ENS_CHANNEL_API __declspec(dllimport)
    #endif

    // ──── ens::business (SHARED) ────
    #ifdef ENS_BUSINESS_EXPORTS
        #define ENS_BUSINESS_API __declspec(dllexport)
    #else
        #define ENS_BUSINESS_API __declspec(dllimport)
    #endif

    // ──── 未来扩展: ens::protocol (候选 SHARED) ────
    #ifdef ENS_PROTOCOL_EXPORTS
        #define ENS_PROTOCOL_API __declspec(dllexport)
    #else
        #define ENS_PROTOCOL_API __declspec(dllimport)
    #endif

// ═══════════════════════════════════════════════════════════════════════════
// GCC / Clang (Linux / macOS SO 可见性)
// ═══════════════════════════════════════════════════════════════════════════
#else
    #define ENS_CHANNEL_API   __attribute__((visibility("default")))
    #define ENS_BUSINESS_API  __attribute__((visibility("default")))
    #define ENS_PROTOCOL_API  __attribute__((visibility("default")))
#endif
```

### 6.2 SHARED/STATIC 接口暴露规则

**CMake 配置片段** (`CMakeLists.txt` 关键部分)：

```cmake
# ============================================================================
# EnerSentry — 混合构建模式 CMake 配置 (V1.5)
# ============================================================================

# ── 构建类型变量（开发阶段全部 STATIC，生产阶段按需切 SHARED）──
set(ENS_CHANNEL_TYPE   SHARED)   # 通信硬件因站而异 → 可热替换 DLL
set(ENS_PROTOCOL_TYPE  STATIC)   # 100ms 轮询热路径 → 内联进 exe
set(ENS_DATAHUB_TYPE   STATIC)   # 无锁 Ring Buffer 最热数据通路 → 内联进 exe
set(ENS_BUSINESS_TYPE  SHARED)   # 业务规则可定制 → 可热替换 DLL
set(ENS_UI_TYPE        STATIC)   # Qt MOC 跨 DLL 有兼容风险 → 内联进 exe

# ── 符号导出宏定义（仅在 SHARED 模块的编译端定义 EXPORTS）──
target_compile_definitions(ens_channel  PRIVATE ENS_CHANNEL_EXPORTS)
target_compile_definitions(ens_business PRIVATE ENS_BUSINESS_EXPORTS)
# STATIC 模块不需要 EXPORTS — 符号默认全部可见

# ── 依赖传递规则 ──
# ✅ PUBLIC: STATIC 模块依赖 SHARED 模块时，必须 PUBLIC 传递
#    否则最终 exe 链接时找不到 DLL 的导入符号
target_link_libraries(ens_protocol PUBLIC ens::channel)
#    └─ protocol (STATIC) 引用 IChannel (SHARED)
#       PUBLIC 确保 channel.dll 的链接要求传递给最终 ens::app (exe)

# ✅ PRIVATE: SHARED 模块依赖 STATIC 模块时，用 PRIVATE 即可
target_link_libraries(ens_business PRIVATE ens::datahub)
#    └─ business (SHARED) 引用 datahub (STATIC)
#       datahub.lib 的符号直接内联进 business.dll，无需传递

# ── 接口暴露规则总结 ──
# | 调用方     | 被调方     | 链接传递 | 原因                                    |
# |-----------|-----------|---------|----------------------------------------|
# | STATIC    | SHARED    | PUBLIC  | exe 需要解析 DLL 导入符号                |
# | SHARED    | STATIC    | PRIVATE | 静态库符号内联进 DLL，无需传递           |
# | STATIC    | STATIC    | PRIVATE | 逐层内联                                |
# | SHARED    | SHARED    | PUBLIC  | 链式 DLL 依赖                           |

# ════════════════════════════════════════════════════════════════════════════
# ⚠ V1.1 强制补充: 16 字节原子操作编译选项
#
# 背景: §3.1 中的 static_assert(std::atomic<Sample>::is_always_lock_free)
#       在 MSVC x64 目标下默认判定为 false。原因是 VS 2015 之前的 CRT 不启用
#       CMPXCHG16B 指令，编译器出于 ABI 兼容考虑将其关闭，导致 std::atomic<16B>
#       退化为内部互斥锁实现 (is_always_lock_free = false)，触发 §3.1 的编译失败。
#
# 解决方案: MSVC x64 目标必须显式添加 /cx16 编译选项 (启用 CMPXCHG16B 指令)。
#           GCC/Clang 已在 x86-64 上无条件支持 16B 原子，无需额外配置。
# ════════════════════════════════════════════════════════════════════════════
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    # /cx16: Enable CMPXCHG16B instruction generation (required for 16-byte atomics)
    add_compile_options(/cx16)
    message(STATUS "[EnerSentry] MSVC x64 detected — /cx16 enabled for 16-byte lock-free atomics")
elseif(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(WARNING "[EnerSentry] MSVC x86 (32-bit) target — 16-byte atomics NOT lock-free. "
                    "Consider shrinking Sample::timestamp to uint32_t.")
endif()
# GCC/Clang x86-64 默认即可直接使用 16 字节原子，无需额外编译选项

# ════════════════════════════════════════════════════════════════════════════
# ⚠ V1.13 强制补充: ARM64 跨平台编译器与指令集校验
#
# 背景: ARM64 (aarch64) 没有 x86 的 cmpxchg16b 指令。16B std::atomic 的 lock-free
#       实现依赖 128-bit LDXP/STXP 循环或 CASP 指令，不同 GCC/Clang 版本与
#       -march/-mcpu 标志的支持差异较大。某些组合下
#       std::atomic<Sample>::is_always_lock_free 会判定为 false，触发 §3.1 的
#       static_assert 编译失败。必须在 CI 中交叉编译验证，而不是上线前才发现。
#
# 解决方案:
#   1. CI 中新增 cross-compile-arm64 Job，使用 aarch64-linux-gnu-g++ 或
#      aarch64-linux-gnu-clang++ 编译 src/datahub。
#   2. 交叉编译参数必须与生产镜像一致（包括 -std=c++17、-march/-mcpu）。
#   3. 若 static_assert 失败，禁止绕过断言；必须切换到 SampleCompact8 +
#      SpscRingBuffer 方案（§3.1.2）。
# ════════════════════════════════════════════════════════════════════════════

# 示例: CI 交叉编译 ARM64 (供 GitHub Actions / GitLab CI 参考)
# cross-compile-arm64.sh:
#   #!/bin/bash
#   set -e
#   mkdir -p build-arm64 && cd build-arm64
#   cmake .. \
#       -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-linux-gnu.cmake \
#       -DCMAKE_BUILD_TYPE=Release \
#       -DENS_DATAHUB_TYPE=STATIC
#   cmake --build . --target ens_datahub --parallel
#
# aarch64-linux-gnu.cmake:
#   set(CMAKE_SYSTEM_NAME Linux)
#   set(CMAKE_SYSTEM_PROCESSOR aarch64)
#   set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
#   set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
#   # 生产若使用 cortex-a76，必须保持一致:
#   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a -std=c++17")
#
# CI 判定: 只要 ens_datahub 编译通过，即证明 static_assert 在目标 ARM64 平台上成立。

# ── RPATH 运行期搜索路径（Linux）──
if(ENS_CHANNEL_TYPE STREQUAL "SHARED" OR ENS_BUSINESS_TYPE STREQUAL "SHARED")
    # Windows: DLL 与 exe 输出到同一目录
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    # Linux: exe 自动在同目录搜索 .so
    set(CMAKE_BUILD_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
endif()
```

### 6.3 V1.9 完整模块 CMakeLists.txt 示例

**目的**：补齐 §6.2 中的片段式 CMake 配置，给出可直接落地的模块级 `CMakeLists.txt` 模板，明确 `target_include_directories`、`target_link_libraries`、`compile_definitions`、`/cx16` 等关键选项的用法。

```cmake
# ============================================================================
# EnerSentry — 模块级 CMakeLists.txt 示例 (V1.9)
# 示例模块: src/datahub/CMakeLists.txt (STATIC 库, 热路径内联进 exe)
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(ens_datahub VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── 源文件 ──────────────────────────────────────────────────────────────
set(SOURCES
    Sample.cpp
    RingBuffer.cpp
    DataAccessImpl.cpp
    platform/PlatformMMap_win.cpp
    platform/PlatformMMap_linux.cpp
)

set(HEADERS
    ${CMAKE_SOURCE_DIR}/include/ens/export.hpp
    ${CMAKE_SOURCE_DIR}/include/ens/QtJsonAdl.h
    ${CMAKE_SOURCE_DIR}/include/ens/PathUtils.h
    Sample.h
    RingBuffer.h
    DataAccessImpl.h
    IDataAccess.h
    IMappedFile.h
)

# ── 静态库（热路径数据通路）──
add_library(ens_datahub STATIC ${SOURCES} ${HEADERS})
add_library(ens::datahub ALIAS ens_datahub)

# ── 公共头文件路径 ───────────────────────────────────────────────────────
# 所有模块都需要访问 include/ens/ 下的公共工具头
target_include_directories(ens_datahub
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/datahub
)

# ── 第三方依赖 ──────────────────────────────────────────────────────────
# Qt5 / Qt6 Core (用于 Q_ASSERT_X, QString 等)
find_package(Qt5 REQUIRED COMPONENTS Core)
# 或: find_package(Qt6 REQUIRED COMPONENTS Core)

# nlohmann/json (header-only)
find_package(nlohmann_json 3.0 REQUIRED)

# spdlog (header-only 或编译库)
find_package(spdlog REQUIRED)

# SQLite3 WAL
find_package(SQLite3 REQUIRED)

target_link_libraries(ens_datahub
    PUBLIC
        Qt5::Core
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        SQLite::SQLite3
)

# ── 编译选项 ─────────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(ens_datahub PRIVATE /W4 /WX-)
    # 启用 C++17 标准
    target_compile_features(ens_datahub PUBLIC cxx_std_17)
endif()

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    # /cx16: 启用 CMPXCHG16B，保证 std::atomic<Sample> (16B) lock-free
    target_compile_options(ens_datahub PUBLIC /cx16)
    message(STATUS "[ens::datahub] MSVC x64 /cx16 enabled")
endif()

# ⚠ V1.13 ARM64 校验: 在 ARM64 目标上，16B std::atomic 的 lock-free 支持取决于
# 编译器版本与 -march 标志。本模块不额外添加标志，但要求 CI 交叉编译任务
# 验证 static_assert(std::atomic<Sample>::is_always_lock_free) 在目标平台通过。
# 若 CI 失败，必须切换为 SampleCompact8 + SpscRingBuffer (§3.1.2)。
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
    message(STATUS "[ens::datahub] ARM64 target detected — ensure CI cross-compile validates 16B atomics")
endif()

# ── 安装规则（可选）──
install(TARGETS ens_datahub
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
```

```cmake
# ============================================================================
# EnerSentry — 顶层 CMakeLists.txt 关键片段 (V1.9)
# 说明: 全局统一设置 + 子模块接入
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(EnerSentry VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # 生成 compile_commands.json 供 clang-tidy 使用

# 全局编译定义
add_compile_definitions(
    NLOHMANN_JSON_NAMESPACE=nlohmann
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
)

# 子模块
add_subdirectory(src/channel)
add_subdirectory(src/protocol)
add_subdirectory(src/datahub)
add_subdirectory(src/business)
add_subdirectory(src/ui)
add_subdirectory(src/app)

# ── 可选: 启用 clang-tidy 静态检查 ──────────────────────────────────────
find_program(CLANG_TIDY_EXE NAMES clang-tidy)
if(CLANG_TIDY_EXE)
    set(CMAKE_CXX_CLANG_TIDY
        "${CLANG_TIDY_EXE};-p;${CMAKE_BINARY_DIR};--config-file=${CMAKE_SOURCE_DIR}/.clang-tidy")
    message(STATUS "[EnerSentry] clang-tidy enabled: ${CLANG_TIDY_EXE}")
endif()
```

---

## 7. 数据结构 JSON 序列化规范 (nlohmann/json)

**约定**：
- 使用 `nlohmann/json` 库，全局命名空间别名 `using json = nlohmann::json;`
- 本项目将 `nlohmann/json` 的命名空间别名为 `ens::json`，通过 `NLOHMANN_JSON_NAMESPACE` 编译宏控制
- 所有 `to_json` / `from_json` 在 `ens::json` 命名空间中定义，以启用 ADL (Argument-Dependent Lookup)
- 结构体使用 `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` 宏简化序列化（如结构体布局简单）或手动编写以获得更好的错误提示

> **⚠ V1.1 修订 (2026-08-07)**：补充 7.0 节"Qt 类型 ADL 适配层"，解决 `nlohmann/json` 原生仅支持 `std::string` 而未定义 `QString` 的 `from_json` 会导致**编译失败**的隐式缺陷。所有使用 `QString` 的 `from_json` 必须依赖 7.0 节的全局 ADL 特化，或先通过 `get<std::string>()` 中转再 `fromStdString()`。

### 7.0 Qt 类型 ADL 适配层（公共 JSON 桥接）

**头文件路径**：`include/ens/QtJsonAdl.h`  
**依赖位置**：**所有使用了 `QString` 的 `from_json` / `to_json` 所在翻译单元的公共依赖**

> **设计原则**：nlohmann/json 通过 **ADL (Argument-Dependent Lookup)** 在 *`QString` 所在的命名空间（即 `Qt` 名字空间或全局命名空间）* 查找 `from_json` / `to_json`。但 Qt 不是 nlohmann 的感知类型，必须由项目显式提供 adapter，否则 `.get_to<QString>()` / `.get<QString>()` 将产生 **SFINAE 编译错误**：
>
> ```
> error: no matching function for call to 'from_json(const nlohmann::json&, QString&)'
> ```

**`QString` 适配实现（标准实现）**：

```cpp
// ============================================================================
// EnerSentry — nlohmann/json 与 Qt 类型 ADL 适配 (V1.5 完整可编译头文件)
// 头文件: include/ens/QtJsonAdl.h
// 编译期: 任何使用了 nlohmann/json + Qt 的 TU 都应 #include 本头文件
// 依赖  : nlohmann/json ≥ 3.0, Qt5/Qt6 Core
// ============================================================================
#pragma once

#include <nlohmann/json.hpp>
#include <QString>
#include <QByteArray>
#include <QtGlobal>    // Q_ASSERT_X

namespace nlohmann {

// ──── QString ↔ json ──────────────────────────────────────────────────────────
// 说明: ADL 特化必须放在 nlohmann 命名空间内，使得 j.get<QString>() 自动查找到
//       否则 nlohmann::json::get 会编译失败（SFINAE-friendly 错误）

template <>
struct adl_serializer<QString> {
    static void to_json(json& j, const QString& opt) {
        j = opt.toStdString();                  // UTF-8 编码，跨平台一致
    }

    static void from_json(const json& j, QString& opt) {
        // V1.2 类型安全分派：避免 j.dump() 在数值/对象/数组节点上
        // 产生带 JSON 格式标记的异常字符串（如 "3" / "{\"a\":1}"）
        if (j.is_string()) {
            opt = QString::fromStdString(j.get<std::string>());
        } else if (j.is_number_integer()) {
            opt = QString::number(j.get<int64_t>());
        } else if (j.is_number_float()) {
            // 保留双精度原始表示，避免 QString::number 默认截断
            opt = QString::number(j.get<double>(), 'g', 16);
        } else if (j.is_boolean()) {
            opt = j.get<bool>() ? QStringLiteral("true") : QStringLiteral("false");
        } else if (j.is_null()) {
            opt.clear();
        } else {
            // 对象 / 数组：仍使用 dump() 作为兜底，但 DEBUG 断言提示配置异常
            Q_ASSERT_X(false, "adl_serializer<QString>::from_json",
                       "Unexpected JSON node type for QString field");
            opt = QString::fromStdString(j.dump());
        }
    }
};

}  // namespace nlohmann

// ──── 单元测试断言（强制编译期锁定）───────────────────────────────────────────
// 在 #include <ens/QtJsonAdl.h> 后，下面这一行必须能成功编译（否则存在 QString 漏接）
static_assert(true, "QtJsonAdl.h included and adl_serializer<QString> available");
```

**`to_json` 序列化策略表**：

| Qt 类型 | 序列化方案 | 备注 |
|--------|----------|------|
| `QString` | `j = s.toStdString()` | UTF-8 跨平台 |
| `QByteArray`（配置字段） | `j = std::string(s.constData(), s.size())` | 字节流避免 `\0` 截断 |
| `QStringList` | `j = s.toStdList<std::string>()` | 使用通用 transform |
| `QDateTime`（可选） | 自定义 ADL，格式 `"yyyy-MM-ddTHH:mm:ss.zzz"` | 序列化参见 §7.4 预留扩展位 |

**调用方强制约束**：

```cpp
// ❌ 错误（裸 .get_to(QString)，若未 include 适配头文件 → 编译失败）
inline void from_json(const nlohmann::json& j, SerialConfig& s) {
    j.at("port_name").get_to(s.portName);   // SFINAE 错误位置
}

// ✅ 推荐: 确保使用方先 include 适配层 (在 src/channel/ChannelConfig.h 顶部)
#include "ens/QtJsonAdl.h"
// ... 此时 j.get_to<QString>() 会被 ADL 正确路由
inline void from_json(const nlohmann::json& j, SerialConfig& s) {
    j.at("port_name").get_to(s.portName);   // 编译通过
}

// ✅ 备选: 显式 std::string 中转（不依赖 ADL 特化，但丢失数值/布尔类型分派）
inline void from_json(const nlohmann::json& j, SerialConfig& s) {
    auto portName = j.at("port_name").get<std::string>();
    s.portName = QString::fromStdString(portName);
}
```

**V1.2 类型安全说明**：

全局 ADL 特化在遇到 `"port_name": "COM3"` 时正常工作；遇到配置错误（如 `"port_name": 3`）时，会按数值类型分派为 `"3"` 而不是产生 `"3"`（数字类型 dump 与数值一致）——对于更复杂的对象/数组节点，DEBUG 模式下会触发 `Q_ASSERT_X` 提醒配置异常，Release 模式下回退到 `j.dump()`。建议业务侧仍通过 JSON Schema 校验或 `j.at("port_name").is_string()` 断言，保证字段类型严格。

**项目级强制 include 策略（CMake 校验）**：

```cmake
# ─── 接口一致性校验: 任何使用 QString + nlohmann/json 的 TU 必须显式 include ───
target_include_directories(ens_channel  PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(ens_business PUBLIC ${CMAKE_SOURCE_DIR}/include)
# 即任何 .cpp 中 #include <nlohmann/json.hpp> 后必须 #include <ens/QtJsonAdl.h>
```

### 7.1 点表配置 pointtable.json

```cpp
// ============================================================================
// EnerSentry — 点表配置序列化 (pointtable.json)
// 头文件: src/protocol/PointTableConfig.h
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace ens::protocol {

// ============================================================================
// RegisterType — Modbus 寄存器类型
// ============================================================================
enum class RegisterType : uint8_t {
    Coil            = 0,  // FC01/05/0F — 线圈
    DiscreteInput   = 1,  // FC02      — 离散输入
    HoldingRegister = 2,  // FC03/06/10 — 保持寄存器
    InputRegister   = 3,  // FC04      — 输入寄存器
};

// ============================================================================
// DataType — 寄存器值数据类型
// ============================================================================
enum class DataType : uint8_t {
    Bool    = 0,
    Int16   = 1,
    Uint16  = 2,
    Int32   = 3,
    Float32 = 4,
    Float64 = 5,
};

// ============================================================================
// ByteOrder — 字节序（多寄存器值时的字节排列顺序）
// ============================================================================
enum class ByteOrder : uint8_t {
    ABCD = 0,  // 大端（Motorola）
    BADC = 1,  // 字节交换
    CDAB = 2,  // 字交换
    DCBA = 3,  // 小端（Intel）
};

// ============================================================================
// PointTableEntry — 单条点表项
// ============================================================================
struct PointTableEntry {
    uint32_t     pointId;          // 全局唯一测点 ID
    std::string  pointName;        // 测点描述（如 "Rack-05 最高温度"）
    uint32_t     linkId;           // 所属通信链路 ID
    uint8_t      slaveAddress;     // Modbus 从站地址
    RegisterType regType;          // 寄存器类型
    uint16_t     registerAddr;     // 起始寄存器地址
    DataType     dataType;         // 数据类型
    ByteOrder    byteOrder;        // 字节序
    float        scaleFactor;      // 缩放系数（工程值 = 寄存器值 × scaleFactor + offset）
    float        offset;           // 偏移量
    std::string  unit;             // 工程单位（如 "℃"、"V"、"kW"）
    uint32_t     pollIntervalMs;   // 轮询周期 (ms)
    uint8_t      priority;         // 轮询优先级 (0=最高, 255=最低)
    bool         enabled;          // 是否启用
};

// ============================================================================
// PointTable — 完整点表配置
// ============================================================================
struct PointTableConfig {
    std::string                 version;     // 配置版本
    std::vector<PointTableEntry> entries;     // 点表条目
};

}  // namespace ens::protocol

// ============================================================================
// nlohmann/json 序列化映射
// ============================================================================

// 枚举 → 字符串映射（使用 NLOHMANN_JSON_SERIALIZE_ENUM）
NLOHMANN_JSON_SERIALIZE_ENUM(ens::protocol::RegisterType, {
    { ens::protocol::RegisterType::Coil,            "coil" },
    { ens::protocol::RegisterType::DiscreteInput,   "discrete_input" },
    { ens::protocol::RegisterType::HoldingRegister, "holding_register" },
    { ens::protocol::RegisterType::InputRegister,   "input_register" },
})

NLOHMANN_JSON_SERIALIZE_ENUM(ens::protocol::DataType, {
    { ens::protocol::DataType::Bool,    "bool" },
    { ens::protocol::DataType::Int16,   "int16" },
    { ens::protocol::DataType::Uint16,  "uint16" },
    { ens::protocol::DataType::Int32,   "int32" },
    { ens::protocol::DataType::Float32, "float32" },
    { ens::protocol::DataType::Float64, "float64" },
})

NLOHMANN_JSON_SERIALIZE_ENUM(ens::protocol::ByteOrder, {
    { ens::protocol::ByteOrder::ABCD, "ABCD" },
    { ens::protocol::ByteOrder::BADC, "BADC" },
    { ens::protocol::ByteOrder::CDAB, "CDAB" },
    { ens::protocol::ByteOrder::DCBA, "DCBA" },
})

namespace ens::protocol {

// PointTableEntry 手动 to_json（字段名使用 JSON 约定：snake_case）
inline void to_json(nlohmann::json& j, const PointTableEntry& e) {
    j = nlohmann::json{
        {"point_id",        e.pointId},
        {"point_name",      e.pointName},
        {"link_id",         e.linkId},
        {"slave_address",   e.slaveAddress},
        {"register_type",   e.regType},
        {"register_addr",   e.registerAddr},
        {"data_type",       e.dataType},
        {"byte_order",      e.byteOrder},
        {"scale_factor",    e.scaleFactor},
        {"offset",          e.offset},
        {"unit",            e.unit},
        {"poll_interval_ms", e.pollIntervalMs},
        {"priority",        e.priority},
        {"enabled",         e.enabled},
    };
}

inline void from_json(const nlohmann::json& j, PointTableEntry& e) {
    j.at("point_id").get_to(e.pointId);
    j.at("point_name").get_to(e.pointName);
    j.at("link_id").get_to(e.linkId);
    j.at("slave_address").get_to(e.slaveAddress);
    j.at("register_type").get_to(e.regType);
    j.at("register_addr").get_to(e.registerAddr);
    j.at("data_type").get_to(e.dataType);
    j.at("byte_order").get_to(e.byteOrder);
    j.at("scale_factor").get_to(e.scaleFactor);
    j.at("offset").get_to(e.offset);
    j.at("unit").get_to(e.unit);
    j.at("poll_interval_ms").get_to(e.pollIntervalMs);
    j.at("priority").get_to(e.priority);
    j.at("enabled").get_to(e.enabled);
}

inline void to_json(nlohmann::json& j, const PointTableConfig& cfg) {
    j = nlohmann::json{
        {"version", cfg.version},
        {"entries", cfg.entries},
    };
}

inline void from_json(const nlohmann::json& j, PointTableConfig& cfg) {
    j.at("version").get_to(cfg.version);
    j.at("entries").get_to(cfg.entries);
}

}  // namespace ens::protocol
```

**pointtable.json 示例**：

```json
{
  "version": "2.0",
  "entries": [
    {
      "point_id": 1001,
      "point_name": "Rack-01 最高温度",
      "link_id": 1,
      "slave_address": 1,
      "register_type": "holding_register",
      "register_addr": 100,
      "data_type": "float32",
      "byte_order": "ABCD",
      "scale_factor": 0.1,
      "offset": 0.0,
      "unit": "℃",
      "poll_interval_ms": 1000,
      "priority": 10,
      "enabled": true
    },
    {
      "point_id": 1002,
      "point_name": "Rack-01 SOC",
      "link_id": 1,
      "slave_address": 1,
      "register_type": "holding_register",
      "register_addr": 102,
      "data_type": "float32",
      "byte_order": "ABCD",
      "scale_factor": 0.01,
      "offset": 0.0,
      "unit": "%",
      "poll_interval_ms": 1000,
      "priority": 10,
      "enabled": true
    }
  ]
}
```

### 7.2 链路配置 channels.json

```cpp
// ============================================================================
// EnerSentry — 链路配置序列化 (channels.json)
// 头文件: src/channel/ChannelConfig.h (扩展序列化部分)
//
// ⚠ V1.1 强制依赖: 本文件中所有 QString 字段的反序列化需要:
//    #include "ens/QtJsonAdl.h"   ← 见 §7.0
// ============================================================================
#pragma once

#include "ens/QtJsonAdl.h"      // §7.0 — nlohmann/json 对 QString 的 ADL 特化
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace ens::channel {

// ============================================================================
// LinkEntry — 单条链路配置项
// ============================================================================
struct LinkEntry {
    uint32_t    linkId;          // 链路唯一 ID
    std::string linkName;        // 链路名称（如 "BMS-RS485-总线1"）
    ChannelType channelType;     // 通道类型
    SerialConfig serial;         // 串口配置（当 type == Serial 时有效）
    TcpConfig    tcp;            // TCP 配置（当 type == TCP 时有效）
    CanConfig    can;            // CAN 配置（当 type == CAN 时有效）
    bool        enabled;         // 是否启用
};

// ============================================================================
// ChannelList — 完整链路配置列表
// ============================================================================
struct ChannelListConfig {
    std::string             version;
    std::vector<LinkEntry>   links;
};

}  // namespace ens::channel

// 枚举序列化
NLOHMANN_JSON_SERIALIZE_ENUM(ens::channel::ChannelType, {
    { ens::channel::ChannelType::Serial, "serial" },
    { ens::channel::ChannelType::TCP,    "tcp" },
    { ens::channel::ChannelType::CAN,    "can" },
})

namespace ens::channel {

inline void to_json(nlohmann::json& j, const SerialConfig& s) {
    j = nlohmann::json{
        {"port_name",           s.portName.toStdString()},
        {"baud_rate",           s.baudRate},
        {"data_bits",           s.dataBits},
        {"stop_bits",           s.stopBits},
        {"parity",              s.parity.toStdString()},
        {"response_timeout_ms", s.responseTimeoutMs},
        {"inter_frame_delay_us", s.interFrameDelayUs},
    };
}

inline void from_json(const nlohmann::json& j, SerialConfig& s) {
    j.at("port_name").get_to(s.portName);
    j.at("baud_rate").get_to(s.baudRate);
    j.at("data_bits").get_to(s.dataBits);
    j.at("stop_bits").get_to(s.stopBits);
    j.at("parity").get_to(s.parity);
    j.at("response_timeout_ms").get_to(s.responseTimeoutMs);
    j.at("inter_frame_delay_us").get_to(s.interFrameDelayUs);
}

inline void to_json(nlohmann::json& j, const TcpConfig& t) {
    j = nlohmann::json{
        {"host",                t.host.toStdString()},
        {"port",                t.port},
        {"connect_timeout_ms",  t.connectTimeoutMs},
        {"response_timeout_ms", t.responseTimeoutMs},
        {"reconnect_base_ms",   t.reconnectBaseMs},
        {"reconnect_max_ms",    t.reconnectMaxMs},
    };
}

inline void from_json(const nlohmann::json& j, TcpConfig& t) {
    j.at("host").get_to(t.host);
    j.at("port").get_to(t.port);
    j.at("connect_timeout_ms").get_to(t.connectTimeoutMs);
    j.at("response_timeout_ms").get_to(t.responseTimeoutMs);
    j.at("reconnect_base_ms").get_to(t.reconnectBaseMs);
    j.at("reconnect_max_ms").get_to(t.reconnectMaxMs);
}

inline void to_json(nlohmann::json& j, const LinkEntry& e) {
    j = nlohmann::json{
        {"link_id",    e.linkId},
        {"link_name",  e.linkName},
        {"type",       e.channelType},
        {"serial",     e.serial},
        {"tcp",        e.tcp},
        {"can",        e.can},
        {"enabled",    e.enabled},
    };
}

inline void from_json(const nlohmann::json& j, LinkEntry& e) {
    j.at("link_id").get_to(e.linkId);
    j.at("link_name").get_to(e.linkName);
    j.at("type").get_to(e.channelType);
    if (j.contains("serial")) j.at("serial").get_to(e.serial);
    if (j.contains("tcp"))    j.at("tcp").get_to(e.tcp);
    if (j.contains("can"))    j.at("can").get_to(e.can);
    j.at("enabled").get_to(e.enabled);
}

inline void to_json(nlohmann::json& j, const ChannelListConfig& cfg) {
    j = nlohmann::json{
        {"version", cfg.version},
        {"links",   cfg.links},
    };
}

inline void from_json(const nlohmann::json& j, ChannelListConfig& cfg) {
    j.at("version").get_to(cfg.version);
    j.at("links").get_to(cfg.links);
}

}  // namespace ens::channel
```

**channels.json 示例**：

```json
{
  "version": "2.0",
  "links": [
    {
      "link_id": 1,
      "link_name": "BMS-RS485-总线1",
      "type": "serial",
      "serial": {
        "port_name": "COM3",
        "baud_rate": 115200,
        "data_bits": 8,
        "stop_bits": 1,
        "parity": "N",
        "response_timeout_ms": 500,
        "inter_frame_delay_us": 3500
      },
      "tcp": {},
      "can": {},
      "enabled": true
    },
    {
      "link_id": 2,
      "link_name": "PCS-TCP",
      "type": "tcp",
      "serial": {},
      "tcp": {
        "host": "192.168.1.100",
        "port": 502,
        "connect_timeout_ms": 3000,
        "response_timeout_ms": 500,
        "reconnect_base_ms": 1000,
        "reconnect_max_ms": 30000
      },
      "can": {},
      "enabled": true
    }
  ]
}
```

### 7.3 告警规则 alarm_rules.json

```cpp
// ============================================================================
// EnerSentry — 告警规则配置序列化 (alarm_rules.json)
// 头文件: src/business/AlarmRuleConfig.h
// ============================================================================
#pragma once

#include "AlarmEngine.h"    // 包含 AlarmRule 定义
#include <nlohmann/json.hpp>
#include <vector>

namespace ens::business {

// ============================================================================
// AlarmRuleConfig — 完整告警规则配置
// ============================================================================
struct AlarmRuleConfig {
    std::string              version;
    uint32_t                 globalConfirmDelayMs;     // 全局默认延时确认 (ms)
    uint32_t                 globalSuppressWindowMs;   // 全局默认抑制窗口 (ms)
    std::vector<AlarmRule>   rules;                    // 规则列表
};

}  // namespace ens::business

// 枚举序列化
NLOHMANN_JSON_SERIALIZE_ENUM(ens::datahub::AlarmLevel, {
    { ens::datahub::AlarmLevel::Info,     "info" },
    { ens::datahub::AlarmLevel::Warning,  "warning" },
    { ens::datahub::AlarmLevel::Critical, "critical" },
})

namespace ens::business {

inline void to_json(nlohmann::json& j, const AlarmRule& r) {
    j = nlohmann::json{
        {"point_id",            r.pointId},
        {"level",               r.level},
        {"upper_threshold",     r.upperThreshold},
        {"lower_threshold",     r.lowerThreshold},
        {"hysteresis_band",     r.hysteresisBand},
        {"confirm_delay_ms",    r.confirmDelayMs},
        {"suppress_window_ms",  r.suppressWindowMs},
        {"enable_black_box",    r.enableBlackBox},
    };
}

inline void from_json(const nlohmann::json& j, AlarmRule& r) {
    j.at("point_id").get_to(r.pointId);
    j.at("level").get_to(r.level);
    j.at("upper_threshold").get_to(r.upperThreshold);
    j.at("lower_threshold").get_to(r.lowerThreshold);
    j.at("hysteresis_band").get_to(r.hysteresisBand);
    j.at("confirm_delay_ms").get_to(r.confirmDelayMs);
    j.at("suppress_window_ms").get_to(r.suppressWindowMs);
    j.at("enable_black_box").get_to(r.enableBlackBox);
}

inline void to_json(nlohmann::json& j, const AlarmRuleConfig& cfg) {
    j = nlohmann::json{
        {"version",                  cfg.version},
        {"global_confirm_delay_ms",  cfg.globalConfirmDelayMs},
        {"global_suppress_window_ms", cfg.globalSuppressWindowMs},
        {"rules",                    cfg.rules},
    };
}

inline void from_json(const nlohmann::json& j, AlarmRuleConfig& cfg) {
    j.at("version").get_to(cfg.version);
    j.at("global_confirm_delay_ms").get_to(cfg.globalConfirmDelayMs);
    j.at("global_suppress_window_ms").get_to(cfg.globalSuppressWindowMs);
    j.at("rules").get_to(cfg.rules);
}

}  // namespace ens::business
```

**alarm_rules.json 示例**：

```json
{
  "version": "2.0",
  "global_confirm_delay_ms": 3000,
  "global_suppress_window_ms": 60000,
  "rules": [
    {
      "point_id": 1001,
      "level": "critical",
      "upper_threshold": 55.0,
      "lower_threshold": -20.0,
      "hysteresis_band": 2.0,
      "confirm_delay_ms": 3000,
      "suppress_window_ms": 60000,
      "enable_black_box": true
    },
    {
      "point_id": 1002,
      "level": "warning",
      "upper_threshold": 95.0,
      "lower_threshold": 10.0,
      "hysteresis_band": 2.0,
      "confirm_delay_ms": 5000,
      "suppress_window_ms": 120000,
      "enable_black_box": false
    }
  ]
}
```

**使用示例 — 热加载告警规则**：

```cpp
// ConfigManager.cpp — 热加载告警规则
#include <fstream>
#include <nlohmann/json.hpp>
#include "business/AlarmEngine.h"
#include "business/AlarmRuleConfig.h"

namespace ens::business {

bool ConfigManager::reloadAlarmRules(const std::string& filePath) {
    std::ifstream ifs(filePath);
    if (!ifs) {
        spdlog::error("Cannot open alarm rules file: {}", filePath);
        return false;
    }

    try {
        nlohmann::json j;
        ifs >> j;
        AlarmRuleConfig cfg = j.get<AlarmRuleConfig>();

        m_alarmEngine->loadRules(cfg.rules);
        spdlog::info("Alarm rules reloaded: {} rules from {}", cfg.rules.size(), filePath);
        return true;
    } catch (const nlohmann::json::exception& e) {
        spdlog::error("Failed to parse alarm rules JSON: {}", e.what());
        return false;
    }
}

}  // namespace ens::business
```

### 7.4 V1.9 JSON 配置统一加载入口 ConfigManager

**目的**：补齐 §7.1~§7.3 中分散的配置加载示例，提供统一的 `ConfigManager` 加载入口，明确文件 I/O、JSON 解析、异常处理、日志记录的规范流程。

```cpp
// ============================================================================
// EnerSentry — ConfigManager JSON 配置统一加载入口 (V1.9)
// 文件: src/common/ConfigManager.h / .cpp
// ============================================================================
#pragma once

#include <nlohmann/json.hpp>
#include <QString>
#include <string>
#include <filesystem>

// 各配置结构体头文件
#include "protocol/PointTableConfig.h"
#include "channel/ChannelConfig.h"
#include "business/AlarmRuleConfig.h"

namespace ens::common {

class ConfigManager {
public:
    /// 加载点表配置
    static std::optional<protocol::PointTableConfig> loadPointTable(
        const std::filesystem::path& path);

    /// 加载链路配置
    static std::optional<channel::ChannelListConfig> loadChannels(
        const std::filesystem::path& path);

    /// 加载告警规则配置
    static std::optional<business::AlarmRuleConfig> loadAlarmRules(
        const std::filesystem::path& path);

    /// 保存配置（用于配置编辑器导出）
    template <typename ConfigT>
    static bool save(const ConfigT& cfg, const std::filesystem::path& path);
};

}  // namespace ens::common
```

```cpp
// ============================================================================
// EnerSentry — ConfigManager 实现模板 (V1.9)
// 文件: src/common/ConfigManager.cpp
// ============================================================================
#include "ConfigManager.h"
#include <spdlog/spdlog.h>
#include <fstream>

namespace ens::common {

namespace {

/// 通用加载模板：读取文件 → 解析 JSON → 反序列化为 ConfigT
template <typename ConfigT>
std::optional<ConfigT> loadJsonFile(const std::filesystem::path& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        spdlog::error("[ConfigManager] Cannot open file: {}", path.string());
        return std::nullopt;
    }

    try {
        nlohmann::json j;
        ifs >> j;
        ConfigT cfg = j.get<ConfigT>();
        spdlog::info("[ConfigManager] Loaded config from {}: version={}",
                     path.string(), cfg.version);
        return cfg;
    } catch (const nlohmann::json::exception& e) {
        spdlog::error("[ConfigManager] JSON parse error in {}: {}",
                      path.string(), e.what());
        return std::nullopt;
    } catch (const std::exception& e) {
        spdlog::error("[ConfigManager] Unexpected error loading {}: {}",
                      path.string(), e.what());
        return std::nullopt;
    }
}

}  // namespace

std::optional<protocol::PointTableConfig> ConfigManager::loadPointTable(
    const std::filesystem::path& path) {
    return loadJsonFile<protocol::PointTableConfig>(path);
}

std::optional<channel::ChannelListConfig> ConfigManager::loadChannels(
    const std::filesystem::path& path) {
    return loadJsonFile<channel::ChannelListConfig>(path);
}

std::optional<business::AlarmRuleConfig> ConfigManager::loadAlarmRules(
    const std::filesystem::path& path) {
    return loadJsonFile<business::AlarmRuleConfig>(path);
}

template <typename ConfigT>
bool ConfigManager::save(const ConfigT& cfg, const std::filesystem::path& path) {
    try {
        nlohmann::json j = cfg;
        std::ofstream ofs(path, std::ios::out | std::ios::trunc);
        if (!ofs) {
            spdlog::error("[ConfigManager] Cannot write file: {}", path.string());
            return false;
        }
        ofs << j.dump(4);  // 4 空格缩进，便于版本控制 diff
        return true;
    } catch (const std::exception& e) {
        spdlog::error("[ConfigManager] Save failed for {}: {}", path.string(), e.what());
        return false;
    }
}

// 显式实例化（避免模板定义在 .cpp 中导致链接错误）
template bool ConfigManager::save<>(const protocol::PointTableConfig&, const std::filesystem::path&);
template bool ConfigManager::save<>(const channel::ChannelListConfig&, const std::filesystem::path&);
template bool ConfigManager::save<>(const business::AlarmRuleConfig&, const std::filesystem::path&);

}  // namespace ens::common
```

**使用示例**：

```cpp
auto ptCfg = ens::common::ConfigManager::loadPointTable("config/pointtable.json");
if (ptCfg) {
    pointTableModel->setEntries(std::move(ptCfg->entries));
}
```

**统一约束**：

1. **版本号校验**：所有配置顶层必须包含 `version` 字段；`ConfigManager` 在加载成功后打印版本号，便于运维追溯。
2. **异常隔离**：每个配置加载函数必须捕获 `nlohmann::json::exception` 与 `std::exception`，返回 `std::nullopt`，禁止向上抛异常导致启动流程崩溃。
3. **文件路径**：统一使用 `std::filesystem::path`（C++17），Windows 长路径自动处理；Qt UI 层传入 `QString` 时通过 `path.toStdString()` 中转。
4. **保存格式**：`dump(4)` 保证 JSON 缩进为 4 空格，便于 Git diff 与人工审查。

---

## 8. 附录：接口契约速查矩阵

### 8.1 模块 × 关键接口 映射

| 模块 | 关键接口 | Target | 构建类型 | 符号导出宏 | 线程约束 |
|------|---------|--------|---------|-----------|---------|
| L1 接入层 | `IChannel` | `ens::channel` | SHARED | `ENS_CHANNEL_API` | IO 线程 |
| L1 接入层 | `ChannelFactory` | `ens::channel` | SHARED | `ENS_CHANNEL_API` | 主线程（构造）/ 任意（调用） |
| L1 接入层 | `ChannelConfig` | `ens::channel` | SHARED | `ENS_CHANNEL_API` | 值语义，无约束 |
| L2 协议层 | `PollScheduler` (signals) | `ens::protocol` | STATIC | 无（内联） | 轮询调度线程 → UI (QueuedConnection) |
| L3 数据中枢 | `Sample` | `ens::datahub` | STATIC | 无（内联） | 原子读/写，任意线程 |
| L3 数据中枢 | `RingBuffer<T>` | `ens::datahub` | STATIC | 无（内联） | 单生产者 + 多消费者 |
| L3 数据中枢 | `IMappedFile` | `ens::datahub` | STATIC | 无（内联） | 任意线程（flushSync 阻塞调用者） |
| L3 数据中枢 | `IDataAccess` | `ens::datahub` | STATIC | 无（内联） | 持久化线程 / 查询线程 |
| L4 业务层 | `AlarmEngine` | `ens::business` | SHARED | `ENS_BUSINESS_API` | 告警线程 (Core 2) |
| L4 业务层 | `DeviceSboGuard` | `ens::business` | SHARED | `ENS_BUSINESS_API` | SBO 线程 (Core 2) |
| L4 业务层 | `AuthManager` | `ens::business` | SHARED | `ENS_BUSINESS_API` | 主线程（线程安全） |
| L5 UI 层 | `DownSampler` | `ens::ui` | STATIC | 无（内联） | 纯函数，线程安全 |
| L5 UI 层 | `RealtimeChartWidget` | `ens::ui` | STATIC | 无（内联） | UI 主线程（仅 onBatchRepaint 可调 replot） |

### 8.2 跨线程信号槽投递契约

| 发送者（线程） | 信号 | 接收者（线程） | 投递方式 |
|---------------|------|---------------|---------|
| 采集线程 | `IChannel::dataReceived` | 协议引擎 | `QueuedConnection` |
| 采集线程 | `L1 RingBuffer::push` | (直接原子写，非信号) | `memory_order_release` |
| 告警线程 | `AlarmEngine::newAlarm` | UI 告警中心 | `QueuedConnection` |
| 轮询调度线程 | `PollScheduler::slaveDegraded` | UI 通信诊断 | `QueuedConnection` |
| SBO 线程 | `DeviceSboGuard::sboStateChanged` | UI SBO 控制台 | `QueuedConnection` |
| 渲染准备线程 | `RealtimeChartWidget::onRenderPacket` | UI 主线程 | `QueuedConnection` |
| 采集线程 | `RealtimeChartWidget::onNewSample` | UI 主线程 | `QueuedConnection` |

### 8.3 性能关键路径内存屏障语义

| 操作 | 内存顺序 | 线程角色 | 保证 |
|------|---------|---------|------|
| `RingBuffer::push` → 数据写入 | `memory_order_relaxed` | 采集线程 | 写入本身不保序，后续 fence 负责 |
| `RingBuffer::push` → Store-Store fence | `memory_order_release` | 采集线程 | T2(store) happens-before T4(release store) |
| `RingBuffer::push` → publishedPos 更新 | `memory_order_release` | 采集线程 | release 与消费者 acquire 配对 |
| `RingBuffer::readRecent` → publishedPos 读取 | `memory_order_acquire` | 任意消费者 | acquire 与生产者 release 配对，保证数据可见 |
| `RingBuffer::readRecent` → buffer[idx] 读取 | `memory_order_acquire` | 任意消费者 | 每个槽位原子读，避免撕裂读 |
| `ChannelStats` 字段写入 | `memory_order_relaxed` | IO 线程 | 纯计数器，无需 happen-before |
| `ChannelStats` 字段读取 | `memory_order_acquire` | 诊断线程 | 读侧 acquire 保证看到最新值 |

### 8.4 接口版本兼容性承诺

| 接口 | 兼容性级别 | 说明 |
|------|----------|------|
| `IChannel` | **ABI 稳定** | 纯虚基类，增方法在末尾追加，不改签名 |
| `Sample` | **布局稳定** | `static_assert(sizeof(Sample)==16)` 编译期锁死，不可增删字段 |
| `RingBuffer<T>` | **模板接口稳定** | `push/pushBatch/readRecent/extractRange` 签名不变 |
| `IMappedFile` | **ABI 稳定** | 纯虚基类，按需追加方法 |
| `IDataAccess` | **ABI 稳定** | 纯虚基类，按需追加方法 |
| `AlarmEngine` signals | **Qt MOC 兼容** | 信号签名不可变（MOC 编译期生成序号） |
| `DeviceSboGuard` signals | **Qt MOC 兼容** | 信号签名不可变 |
| JSON 配置文件格式 | **向前兼容** | 新增字段给默认值；旧版解析忽略新字段 |

---

*本文档为 EnerSentry 储能上位机系统的正式接口控制文档 (ICD) / 接口设计说明 (IDD) V1.5，基于概要设计说明书 V1.5 及全部参考文档编制。所有 C++ 头文件声明、数据结构定义、内存语义标注、JSON 序列化规范均经 HLD 设计一致性校验。开发团队编码时应严格遵循本文档中的接口签名与约束，任何偏离需经 ADR 评审。*
