// src/protocol/PointTable.cpp
// L2 协议引擎 ── 点表解析器实现（ENS-LLD-100 §4.4 / ENS-DEV-GUIDE §3A 3.1.5）。
//
// 实现要点：
//   * JSON 解析经 nlohmann_json（已在 ens::3rdparty 接口库透传）；
//   * schemaVersion 校验:仅接受 "1.1" (当前 sample.json) / "1.0" (兼容旧 sample,降级但 warn 不拒)；
//   * enum class 字符串映射：regType / dataType / byteOrder 三组,任一未识别抛 std::runtime_error；
//   * registerAddr JSON 既支持十进制整数也支持 "0x...." 十六进制字符串(加载器按 uint16_t 解析)；
//   * (slave,addr) → pointId 索引用 64-bit key 一次哈希查找（O(1)），热路径无 RTTI；
//   * 字节序重组 + 工程值还原：decodeToEngineering 只对 Floats/Int 做字节解释,
//     Bool 走 coil 位图（3.1.3 ModbusEngine 处理）。

#include "PointTable.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
    #include <stdio.h>   // _wfopen
#endif

namespace ens::protocol {

namespace {

using nlohmann::json;

// ── 字符串 ↔ enum class 映射（容错：未识别 → runtime_error）──
RegisterType parseRegType(const std::string& s) {
    if (s == "Coil")            return RegisterType::Coil;
    if (s == "DiscreteInput")   return RegisterType::DiscreteInput;
    if (s == "HoldingRegister") return RegisterType::HoldingRegister;
    if (s == "InputRegister")   return RegisterType::InputRegister;
    throw std::runtime_error("PointTable: unknown regType='" + s + "'");
}
DataType parseDataType(const std::string& s) {
    if (s == "Bool")    return DataType::Bool;
    if (s == "Int16")   return DataType::Int16;
    if (s == "Uint16")  return DataType::Uint16;
    if (s == "Int32")   return DataType::Int32;
    if (s == "Float32") return DataType::Float32;
    if (s == "Float64") return DataType::Float64;
    throw std::runtime_error("PointTable: unknown dataType='" + s + "'");
}
ByteOrder parseByteOrder(const std::string& s) {
    if (s == "ABCD") return ByteOrder::ABCD;
    if (s == "BADC") return ByteOrder::BADC;
    if (s == "CDAB") return ByteOrder::CDAB;
    if (s == "DCBA") return ByteOrder::DCBA;
    throw std::runtime_error("PointTable: unknown byteOrder='" + s + "'");
}

uint16_t parseRegisterAddr(const json& j) {
    // 支持 "registerAddr": 4096 或 "registerAddr": "0x1000"
    if (j.is_number_unsigned())  return j.get<uint16_t>();
    if (j.is_number_integer())   return j.get<uint16_t>();
    if (j.is_string()) {
        const std::string& s = j.get_ref<const std::string&>();
        if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            return static_cast<uint16_t>(std::stoul(s, nullptr, 16));
        }
        return static_cast<uint16_t>(std::stoul(s));
    }
    throw std::runtime_error("PointTable: registerAddr must be integer or hex string");
}

}  // namespace

// ── 加载入口 ──────────────────────────────────────────────────────────────

std::shared_ptr<PointTable> PointTable::loadFromJsonFile(const std::string& path) {
    return loadFromJsonFile(std::filesystem::path(path));
}

std::shared_ptr<PointTable> PointTable::loadFromJsonFile(const std::filesystem::path& path) {
    // Windows 中文路径:path.string()/wstring() 经 ANSI code page 转换会抛 system_error,
    // 直接走 path.c_str()(UTF-16 native) 给 _wfopen,完全绕开窄/宽转换。
    std::string content;
    const std::string display = path.generic_u8string();
#if defined(_WIN32)
    FILE* fp = ::_wfopen(path.c_str(), L"rb");
    if (!fp) {
        throw std::runtime_error("PointTable: cannot open file '" + display + "'");
    }
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
        content.append(buf, n);
    }
    std::fclose(fp);
#else
    std::ifstream is(path);
    if (!is.is_open()) {
        throw std::runtime_error("PointTable: cannot open file '" + display + "'");
    }
    std::ostringstream oss;
    oss << is.rdbuf();
    content = oss.str();
#endif

    json root;
    try {
        auto j = nlohmann::json::parse(content.begin(), content.end());
        root = std::move(j);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("PointTable: JSON parse error in '") +
                                 display + "': " + e.what());
    }
    auto tbl = std::make_shared<PointTable>();
    if (!root.contains("meta") || !root["meta"].contains("schemaVersion")) {
        throw std::runtime_error("PointTable: JSON missing meta.schemaVersion");
    }
    const std::string sv = root["meta"]["schemaVersion"].get<std::string>();
    if (sv != "1.1" && sv != "1.0") {
        throw std::runtime_error("PointTable: unsupported schemaVersion='" + sv +
                                 "' (expected 1.0 or 1.1)");
    }
    if (!root.contains("points") || !root["points"].is_array()) {
        throw std::runtime_error("PointTable: JSON missing 'points' array");
    }
    for (const auto& pj : root["points"]) {
        PointRuntime pr;
        pr.pointId        = pj.at("pointId").get<uint32_t>();
        pr.slaveAddress   = pj.at("slaveAddress").get<uint8_t>();
        pr.linkId         = pj.value("linkId", static_cast<uint32_t>(pr.slaveAddress));
        pr.regType        = parseRegType(pj.at("regType").get<std::string>());
        pr.dataType       = parseDataType(pj.at("dataType").get<std::string>());
        pr.byteOrder      = parseByteOrder(pj.at("byteOrder").get<std::string>());
        pr.registerAddr   = parseRegisterAddr(pj.at("registerAddr"));
        pr.scaleFactor    = pj.value("scaleFactor", 1.0f);
        pr.offset         = pj.value("offset", 0.0f);
        pr.pollIntervalMs = pj.value("pollIntervalMs", 1000u);
        pr.priority       = pj.value("priority", static_cast<uint8_t>(1));
        pr.enabled        = pj.value("enabled", true);
        pr.pointName      = pj.value("pointName", std::string{});
        pr.unit           = pj.value("unit", std::string{});
        // 索引填充(重复检测)
        if (tbl->m_byPointId.find(pr.pointId) != tbl->m_byPointId.end()) {
            throw std::runtime_error("PointTable: duplicate pointId=" +
                                     std::to_string(pr.pointId));
        }
        const uint64_t key = kAddrKey(pr.slaveAddress, pr.registerAddr);
        if (tbl->m_byAddr.find(key) != tbl->m_byAddr.end()) {
            throw std::runtime_error("PointTable: duplicate (slave,addr)=(" +
                std::to_string(pr.slaveAddress) + "," +
                std::to_string(pr.registerAddr) + ") for pointId=" +
                std::to_string(pr.pointId));
        }
        tbl->m_byPointId.emplace(pr.pointId, pr);
        tbl->m_byAddr.emplace(key, pr.pointId);
        tbl->m_bySlave[pr.slaveAddress].push_back(pr.pointId);
    }
    // m_bySlave 内按 registerAddr 升序(供 allOnSlave 直接返回)
    for (auto& [slave, ids] : tbl->m_bySlave) {
        (void)slave;
        std::sort(ids.begin(), ids.end(), [&tbl](uint32_t a, uint32_t b) {
            return tbl->m_byPointId.at(a).registerAddr <
                   tbl->m_byPointId.at(b).registerAddr;
        });
    }
    return tbl;
}

// ── 查询 ──────────────────────────────────────────────────────────────────

const PointRuntime* PointTable::resolve(uint8_t slaveAddress, uint16_t registerAddr) const noexcept {
    const auto it = m_byAddr.find(kAddrKey(slaveAddress, registerAddr));
    if (it == m_byAddr.end()) return nullptr;
    const auto pit = m_byPointId.find(it->second);
    return (pit != m_byPointId.end()) ? &pit->second : nullptr;
}

const PointRuntime* PointTable::pointIdOf(uint32_t pointId) const noexcept {
    const auto it = m_byPointId.find(pointId);
    return (it != m_byPointId.end()) ? &it->second : nullptr;
}

std::vector<const PointRuntime*> PointTable::allOnSlave(uint8_t slaveAddress) const noexcept {
    std::vector<const PointRuntime*> out;
    const auto it = m_bySlave.find(slaveAddress);
    if (it == m_bySlave.end()) return out;
    out.reserve(it->second.size());
    for (uint32_t pid : it->second) {
        const auto pit = m_byPointId.find(pid);
        if (pit != m_byPointId.end()) out.push_back(&pit->second);
    }
    return out;
}

std::vector<const PointRuntime*> PointTable::allPoints() const noexcept {
    std::vector<const PointRuntime*> out;
    out.reserve(m_byPointId.size());
    for (const auto& [pid, pr] : m_byPointId) {
        (void)pid;
        out.push_back(&pr);
    }
    return out;
}

// ── 字节序重组 / 工程值还原 ─────────────────────────────────────────────

size_t PointTable::registerCountFor(DataType dt) noexcept {
    switch (dt) {
        case DataType::Bool:    return 1;   // 按 Modbus coil/discrete 位打包约定
        case DataType::Int16:   return 1;
        case DataType::Uint16:  return 1;
        case DataType::Int32:   return 2;
        case DataType::Float32: return 2;
        case DataType::Float64: return 4;
    }
    return 0;
}

size_t PointTable::reassembleBytes(const uint16_t* regs, size_t regCount,
                                   ByteOrder order, uint8_t* out, size_t outCap) noexcept {
    if (regs == nullptr || out == nullptr || regCount == 0 || regCount > 4) return 0;
    const size_t need = regCount * 2;
    if (outCap < need) return 0;

    // 把 regs 按 Modbus 默认大端拆为 2*regCount 字节（buf[0..need)）
    uint8_t buf[8] = {};
    for (size_t i = 0; i < regCount; ++i) {
        buf[2 * i + 0] = static_cast<uint8_t>(regs[i] >> 8);  // A
        buf[2 * i + 1] = static_cast<uint8_t>(regs[i] & 0xFF); // B
    }

    // byteOrder 重组 — ABCD = [A,B,C,D]; CDAB = dword halves swap; BADC = byte swap within word; DCBA = full reverse
    switch (order) {
        case ByteOrder::ABCD:
            // 字节序就是 Modbus 大端,直接透传
            std::memcpy(out, buf, need);
            break;

        case ByteOrder::CDAB:
            // [A,B,C,D] → [C,D,A,B]:dword 内 16-bit word 序反转。
            //   regCount==2 → 直接前后两 word 互换;
            //   regCount==4 → 按 32-bit dword 粒度两两互换(即 [A..H]→[E,F,G,H,A,B,C,D],
            //                 64-bit 值的 word 级整体反转为 DCBA,CDAB 保持 dword 内 word 交换语义)。
            for (size_t i = 0; i < regCount / 2; ++i) {
                const size_t src = 2 * i;
                const size_t dst = 2 * (i + regCount / 2);
                out[dst + 0] = buf[src + 0];
                out[dst + 1] = buf[src + 1];
                out[src + 0] = buf[dst + 0];
                out[src + 1] = buf[dst + 1];
            }
            break;

        case ByteOrder::BADC:
            // [A,B,C,D] → [B,A,D,C]:每个 16-bit word 内字节反
            for (size_t i = 0; i < need; i += 2) {
                out[i + 0] = buf[i + 1];
                out[i + 1] = buf[i + 0];
            }
            break;

        case ByteOrder::DCBA:
            // [A,B,C,D] → [D,C,B,A]:整体反转
            for (size_t i = 0; i < need; ++i) out[i] = buf[need - 1 - i];
            break;
    }
    return need;
}

double PointTable::decodeToEngineering(const uint8_t* bytes, size_t len,
                                       DataType dt, float scale, float offset) const noexcept {
    if (bytes == nullptr) return 0.0;
    // bytes[] 是 reassembleBytes 输出的大端字节流(MSB 在前);x86 小端机器上
    // 直接 memcpy 到 int32_t/float 会按本机端序解释 → 错值。先按大端组装 host-order
    // uint32_t/uint64_t 再 memcpy。
    auto beToHost32 = [](const uint8_t* b) -> uint32_t {
        return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) |
               (uint32_t(b[2]) << 8)  |  uint32_t(b[3]);
    };
    auto beToHost64 = [](const uint8_t* b) -> uint64_t {
        return (uint64_t(b[0]) << 56) | (uint64_t(b[1]) << 48) |
               (uint64_t(b[2]) << 40) | (uint64_t(b[3]) << 32) |
               (uint64_t(b[4]) << 24) | (uint64_t(b[5]) << 16) |
               (uint64_t(b[6]) << 8)  |  uint64_t(b[7]);
    };
    double v = 0.0;
    switch (dt) {
        case DataType::Int16: {
            if (len < 2) return 0.0;
            const int16_t i = static_cast<int16_t>((bytes[0] << 8) | bytes[1]);
            v = static_cast<double>(i);
            break;
        }
        case DataType::Uint16: {
            if (len < 2) return 0.0;
            const uint16_t u = static_cast<uint16_t>((bytes[0] << 8) | bytes[1]);
            v = static_cast<double>(u);
            break;
        }
        case DataType::Int32: {
            if (len < 4) return 0.0;
            const int32_t i = static_cast<int32_t>(beToHost32(bytes));
            v = static_cast<double>(i);
            break;
        }
        case DataType::Float32: {
            if (len < 4) return 0.0;
            const uint32_t u = beToHost32(bytes);
            float f;
            std::memcpy(&f, &u, 4);
            v = static_cast<double>(f);
            break;
        }
        case DataType::Float64: {
            if (len < 8) return 0.0;
            const uint64_t u = beToHost64(bytes);
            double d;
            std::memcpy(&d, &u, 8);
            v = d;
            break;
        }
        case DataType::Bool:
            v = (len >= 1 && bytes[0] != 0) ? 1.0 : 0.0;
            break;
    }
    return v * static_cast<double>(scale) + static_cast<double>(offset);
}

}  // namespace ens::protocol
