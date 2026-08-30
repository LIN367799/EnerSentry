// src/datahub/platform/PlatformMMap.h
// L3 数据中枢 ── 跨平台内存映射抽象（ENS-LLD-200 §3.6.2 / ADR-20 / HLD §3.2.2.2）。
//
// 设计约束：
//   * 业务代码禁止直接 include <sys/mman.h> / <windows.h> 映射 API,一律经 IMappedFile
//   * close() 必须幂等(UnmapViewOfFile → CloseHandle(map) → CloseHandle(file),二次调用不抛)
//   * flushAsync 不阻塞;flushSync 阻塞落盘(断电前最后一搏,≤20ms)
//   * Windows 显式 FILE_SHARE_READ|WRITE,允许重启时以写模式重开(避免恢复失败)

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ens::datahub::platform {

/// 文件锁定 + 内存映射的跨平台抽象接口
class IMappedFile {
public:
    virtual ~IMappedFile() = default;

    /// 打开(或创建)文件并映射内存
    /// @param path 文件路径;size 目标大小(>0 且文件不存在时创建);readOnly 只读映射
    /// @return true 成功
    virtual bool open(const std::string& path, size_t size, bool readOnly) = 0;

    /// 映射基地址(open 成功后有效;close 后无效)
    virtual void* baseAddress() const = 0;

    /// 映射大小
    virtual size_t size() const = 0;

    /// 异步落盘(不阻塞;内核后台刷写)
    virtual bool flushAsync(size_t offset, size_t length) = 0;

    /// 同步落盘(阻塞;断电前最后一搏)
    virtual bool flushSync(size_t offset, size_t length) = 0;

    /// 关闭映射 + 文件句柄(必须幂等)
    virtual void close() = 0;

    /// 最近错误码(0=OK;3=文件被其他进程锁定)
    virtual int lastError() const = 0;

    /// 文件是否被其他进程锁定(Windows ERROR_SHARING_VIOLATION 场景)
    virtual bool isLockedByOtherProcess() const = 0;
};

/// 工厂:按编译环境自动选择 Win32 / POSIX 实现
std::unique_ptr<IMappedFile> createMappedFile();

}  // namespace ens::datahub::platform
