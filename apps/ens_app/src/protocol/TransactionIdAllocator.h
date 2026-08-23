// src/protocol/TransactionIdAllocator.h
// TCP 16-bit 事务 ID 分配器（ENS-LLD-100 §4.3.6 / DevGuide §2A）。
// 关键点：8KB 位图经 unique_ptr 置于堆上；禁拷贝，移动仅转移指针。

#pragma once

#include <bitset>
#include <memory>
#include <cstdint>

namespace ens::protocol {

class TransactionIdAllocator {
public:
    static constexpr uint16_t INVALID_ID = 0;   // 0 保留为无效，分配从 1 开始

    TransactionIdAllocator() : m_used(std::make_unique<std::bitset<65536>>()) {}

    TransactionIdAllocator(const TransactionIdAllocator&) = delete;
    TransactionIdAllocator& operator=(const TransactionIdAllocator&) = delete;
    TransactionIdAllocator(TransactionIdAllocator&&) = default;
    TransactionIdAllocator& operator=(TransactionIdAllocator&&) = default;

    // 分配 [1,65535] 中最低空闲 ID；耗尽返回 INVALID_ID。
    uint16_t allocate() noexcept {
        for (uint32_t id = 1; id <= 65535; ++id) {   // uint32_t 防 16-bit 回绕死循环
            if (!m_used->test(id)) {
                m_used->set(id);
                return static_cast<uint16_t>(id);
            }
        }
        return INVALID_ID;
    }

    // 释放 ID 使其可复用（0 忽略）。
    void release(uint16_t id) noexcept {
        if (id != INVALID_ID) m_used->reset(id);
    }

    // 清空全部已分配 ID（断链/重连时调用，防 16-bit 回绕错配）。
    void clearInFlight() noexcept { m_used->reset(); }

    // 查询某 ID 是否已分配。
    bool isAllocated(uint16_t id) const noexcept {
        return id != INVALID_ID && m_used->test(id);
    }

private:
    std::unique_ptr<std::bitset<65536>> m_used;   // 8KB 位图置于堆上；0=空闲 1=已分配
};

}  // namespace ens::protocol
