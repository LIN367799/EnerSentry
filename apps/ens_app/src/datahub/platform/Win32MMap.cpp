// src/datahub/platform/Win32MMap.cpp
// L3 数据中枢 ── Windows 内存映射实现（ENS-LLD-200 §3.6.2 / ADR-20）。
//
// 关键点：
//   * CreateFileMapping + MapViewOfFile;文件锁定 = ERROR_SHARING_VIOLATION(映射独占写锁)
//   * close() 幂等:UnmapViewOfFile → CloseHandle(map) → CloseHandle(file)
//   * flushSync = FlushViewOfFile + FlushFileBuffers(断电前最后一搏)
//   * 显式 FILE_SHARE_READ|WRITE:允许重启进程以写模式重开(避免恢复失败)

#ifdef _WIN32
#include "PlatformMMap.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

namespace ens::datahub::platform {

namespace {

/// 文件锁冲突错误码(ERROR_SHARING_VIOLATION)
constexpr int kErrLockedByOther = 3;

}  // namespace

class Win32MMap : public IMappedFile {
public:
    Win32MMap() = default;
    ~Win32MMap() override { close(); }

    bool open(const std::string& path, size_t size, bool readOnly) override {
        close();                                     // 幂等:重开前先清理

        // 文件访问模式:读写(默认)或只读
        const DWORD access    = readOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
        const DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;   // 允许重启重开
        const DWORD creation  = readOnly ? OPEN_EXISTING : OPEN_ALWAYS;

        m_file = CreateFileA(path.c_str(), access, shareMode, nullptr,
                             creation, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_file == INVALID_HANDLE_VALUE) {
            m_lastError = (GetLastError() == ERROR_SHARING_VIOLATION)
                              ? kErrLockedByOther : static_cast<int>(GetLastError());
            m_file = nullptr;
            return false;
        }

        // 文件不存在(新建)时按 size 扩展
        if (!readOnly) {
            LARGE_INTEGER fileSize;
            if (GetFileSizeEx(m_file, &fileSize) && fileSize.QuadPart == 0) {
                LARGE_INTEGER sz;
                sz.QuadPart = static_cast<LONGLONG>(size);
                SetFilePointerEx(m_file, sz, nullptr, FILE_BEGIN);
                SetEndOfFile(m_file);
            }
        }

        const DWORD protect  = readOnly ? PAGE_READONLY : PAGE_READWRITE;
        m_mapping = CreateFileMappingA(m_file, nullptr, protect,
                                       0, 0, nullptr);   // 0 = 全文件映射
        if (m_mapping == nullptr) {
            m_lastError = static_cast<int>(GetLastError());
            CloseHandle(m_file);
            m_file = nullptr;
            return false;
        }

        m_view = MapViewOfFile(m_mapping, readOnly ? FILE_MAP_READ : (FILE_MAP_READ | FILE_MAP_WRITE),
                               0, 0, 0);
        if (m_view == nullptr) {
            m_lastError = static_cast<int>(GetLastError());
            CloseHandle(m_mapping);
            m_mapping = nullptr;
            CloseHandle(m_file);
            m_file = nullptr;
            return false;
        }

        // 记录实际映射大小(新建时 = size;重开时 = 文件实际大小)
        LARGE_INTEGER actual;
        if (GetFileSizeEx(m_file, &actual)) {
            m_size = static_cast<size_t>(actual.QuadPart);
        } else {
            m_size = size;
        }
        m_lastError = 0;
        return true;
    }

    void* baseAddress() const override { return m_view; }
    size_t size() const override { return m_size; }

    bool flushAsync(size_t offset, size_t length) override {
        if (m_view == nullptr) return false;
        // FlushViewOfFile 是同步调用,但 Windows 上 == "落盘";异步语义由调用方(短窗)承担
        return FlushViewOfFile(static_cast<char*>(m_view) + offset, length) != 0;
    }

    bool flushSync(size_t offset, size_t length) override {
        if (m_view == nullptr || m_file == nullptr) return false;
        if (!FlushViewOfFile(static_cast<char*>(m_view) + offset, length)) return false;
        return FlushFileBuffers(m_file) != 0;          // 断电前最后一搏
    }

    void close() override {
        if (m_view != nullptr) {
            UnmapViewOfFile(m_view);
            m_view = nullptr;
        }
        if (m_mapping != nullptr) {
            CloseHandle(m_mapping);
            m_mapping = nullptr;
        }
        if (m_file != nullptr) {
            CloseHandle(m_file);
            m_file = nullptr;
        }
        m_size = 0;
    }

    int lastError() const override { return m_lastError; }

    bool isLockedByOtherProcess() const override {
        return m_lastError == kErrLockedByOther;
    }

private:
    HANDLE m_file    = nullptr;
    HANDLE m_mapping = nullptr;
    void*  m_view    = nullptr;
    size_t m_size    = 0;
    int    m_lastError = 0;
};

std::unique_ptr<IMappedFile> createMappedFile() {
    return std::make_unique<Win32MMap>();
}

}  // namespace ens::datahub::platform

#endif  // _WIN32
