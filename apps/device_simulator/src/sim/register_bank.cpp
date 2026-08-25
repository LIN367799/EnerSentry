// sim/register_bank.cpp —— Track B B2 RegisterBank RCU 快照库实现（ENS-LLD-SIM §4.4 / §5.2）。
//
// 实现要点：
//   * snapshot 用 std::shared_lock：多读者并发不互斥，写者 unique_lock 阻塞
//     读者时间极短（map.find + shared_ptr 拷贝；refcount 增减是 atomic），工程实测
//     100Hz 1000 点轮询无锁竞争。
//   * publish 用 std::unique_lock：写入只动一个 map 项（operator[]/insert + shared_ptr
//     移动赋值），持锁时间 < 1us；旧快照在最后一个持有者释放后自动析构，无需手动管理。
//   * readControl/writeControl 与 publish 共享同一 m_rw（避免 SBO 写与 publish 撞车）：
//     读用 shared_lock（与 snapshot 同序），写用 unique_lock（与 publish 同序）。
//
// 内存序：std::shared_mutex 标准保证多读者并发可见性，无需额外 atomic；shared_ptr 拷贝
// 的引用计数增减由标准库实现保证 atomic。

#include "sim/register_bank.h"

namespace ens::sim {

std::shared_ptr<const SlaveRegset> RegisterBank::snapshot(uint8_t slave) const noexcept {
    std::shared_lock<std::shared_mutex> lock(m_rw);
    const auto it = m_banks.find(slave);
    return (it != m_banks.end()) ? it->second : nullptr;
}

void RegisterBank::publish(uint8_t slave, std::shared_ptr<const SlaveRegset> next) noexcept {
    std::unique_lock<std::shared_mutex> lock(m_rw);
    m_banks[slave] = std::move(next);
}

void RegisterBank::install(uint8_t slave, std::shared_ptr<const SlaveRegset> initial) noexcept {
    // install 与 publish 同语义；提供明确语义便于调用方区分"首次建仓"与"替换快照"
    publish(slave, std::move(initial));
}

uint16_t RegisterBank::readControl(uint8_t slave, uint16_t reg) const noexcept {
    if (reg >= kControlRegCount) return 0;
    std::shared_lock<std::shared_mutex> lock(m_rw);
    const auto it = m_banks.find(slave);
    if (it == m_banks.end() || it->second == nullptr) return 0;
    return it->second->getHolding(reg);
}

void RegisterBank::writeControl(uint8_t slave, uint16_t reg, uint16_t v) noexcept {
    if (reg >= kControlRegCount) return;
    std::unique_lock<std::shared_mutex> lock(m_rw);
    const auto it = m_banks.find(slave);
    if (it == m_banks.end() || it->second == nullptr) return;
    // Copy-on-Write：拷贝出 *non-const* SlaveRegset 副本 → 修改控制字 → 替换原 shared_ptr。
    // readers 在 writeControl 持锁期间仍看到旧快照；writeControl 返回后新快照生效。
    // 这才是真正的 RCU — 任何时刻所有 reader 看到的都是一个不变快照。
    auto copy = std::make_shared<SlaveRegset>(*it->second);
    copy->setHolding(reg, v);
    m_banks[slave] = std::move(copy);
}

size_t RegisterBank::slaveCount() const noexcept {
    std::shared_lock<std::shared_mutex> lock(m_rw);
    return m_banks.size();
}

void RegisterBank::clear() noexcept {
    std::unique_lock<std::shared_mutex> lock(m_rw);
    m_banks.clear();
}

}  // namespace ens::sim