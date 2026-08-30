// src/datahub/platform/PosixMMap.cpp
// L3 数据中枢 ── POSIX 内存映射实现（ENS-LLD-200 §3.6.2 / ADR-20）。
//
// 关键点：
//   * open + ftruncate + mmap(PROT_READ|WRITE, MAP_SHARED)
//   * close() 幂等:munmap → close(fd)
//   * flushSync = msync(MS_SYNC);flushAsync = msync(MS_ASYNC)
//   * POSIX 无"其他进程锁定"语义,isLockedByOtherProcess 恒 false

#ifndef _WIN32
#include "PlatformMMap.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ens::datahub::platform {

class PosixMMap : public IMappedFile {
public:
    PosixMMap() = default;
    ~PosixMMap() override { close(); }

    bool open(const std::string& path, size_t size, bool readOnly) override {
        close();                                     // 幂等:重开前先清理

        const int flags = readOnly ? O_RDONLY : (O_RDWR | O_CREAT);
        m_fd = ::open(path.c_str(), flags, 0644);
        if (m_fd < 0) {
            m_lastError = errno;
            m_fd = -1;
            return false;
        }

        if (!readOnly) {
            struct stat st;
            if (fstat(m_fd, &st) == 0 && st.st_size == 0) {
                if (ftruncate(m_fd, static_cast<off_t>(size)) != 0) {
                    m_lastError = errno;
                    ::close(m_fd);
                    m_fd = -1;
                    return false;
                }
            }
        }

        const int prot = readOnly ? PROT_READ : (PROT_READ | PROT_WRITE);
        m_base = ::mmap(nullptr, size, prot, MAP_SHARED, m_fd, 0);
        if (m_base == MAP_FAILED) {
            m_lastError = errno;
            m_base = nullptr;
            ::close(m_fd);
            m_fd = -1;
            return false;
        }
        m_size = size;
        m_lastError = 0;
        return true;
    }

    void* baseAddress() const override { return m_base; }
    size_t size() const override { return m_size; }

    bool flushAsync(size_t offset, size_t length) override {
        if (m_base == nullptr) return false;
        return ::msync(static_cast<char*>(m_base) + offset, length, MS_ASYNC) == 0;
    }

    bool flushSync(size_t offset, size_t length) override {
        if (m_base == nullptr) return false;
        return ::msync(static_cast<char*>(m_base) + offset, length, MS_SYNC) == 0;
    }

    void close() override {
        if (m_base != nullptr) {
            ::munmap(m_base, m_size);
            m_base = nullptr;
        }
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
        m_size = 0;
    }

    int lastError() const override { return m_lastError; }
    bool isLockedByOtherProcess() const override { return false; }

private:
    int    m_fd   = -1;
    void*  m_base = nullptr;
    size_t m_size = 0;
    int    m_lastError = 0;
};

std::unique_ptr<IMappedFile> createMappedFile() {
    return std::make_unique<PosixMMap>();
}

}  // namespace ens::datahub::platform

#endif  // !_WIN32
