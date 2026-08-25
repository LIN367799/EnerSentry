// src/core/point_table.cpp —— 见 point_table.h

#include "point_table.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ens::sim {

using json = nlohmann::json;

// ── 字符串 → 枚举映射（schemaVersion: 1.0, SIM-IMP §3.2）───────────────
namespace {
RegisterType parseRegisterType(const std::string& s) {
    if (s == "HoldingRegister") return RegisterType::HoldingRegister;
    if (s == "InputRegister")   return RegisterType::InputRegister;
    if (s == "Coil")            return RegisterType::Coil;
    if (s == "DiscreteInput")   return RegisterType::DiscreteInput;
    throw std::runtime_error("SimPointTable: unknown regType '" + s + "'");
}
DataType parseDataType(const std::string& s) {
    if (s == "Bool")    return DataType::Bool;
    if (s == "Int16")   return DataType::Int16;
    if (s == "Uint16")  return DataType::Uint16;
    if (s == "Int32")   return DataType::Int32;
    if (s == "Float32") return DataType::Float32;
    if (s == "Float64") return DataType::Float64;
    throw std::runtime_error("SimPointTable: unknown dataType '" + s + "'");
}
ByteOrder parseByteOrder(const std::string& s) {
    if (s == "ABCD") return ByteOrder::ABCD;
    if (s == "BADC") return ByteOrder::BADC;
    if (s == "CDAB") return ByteOrder::CDAB;
    if (s == "DCBA") return ByteOrder::DCBA;
    throw std::runtime_error("SimPointTable: unknown byteOrder '" + s + "'");
}

uint16_t parseHexOrDec(const std::string& s) {
    // SIM-IMP §3.2:registerAddr = 十进制或 0x 十六进制
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        return static_cast<uint16_t>(std::stoul(s.substr(2), nullptr, 16));
    }
    return static_cast<uint16_t>(std::stoul(s, nullptr, 10));
}
}  // namespace

// ── 主入口 ──────────────────────────────────────────────────────────────

std::shared_ptr<SimPointTable> SimPointTable::loadFromJsonFile(
    const std::filesystem::path& path) {
    // ⚠ 与主程序 PointTable 走相同策略（Phase 2 3.1.5 实测坑 #6）：
    //   MSVC std::ifstream(path.wstring()) 内部走 fopen() 转 ANSI,在中文路径下抛
    //   "No mapping for the Unicode character" system_error。
    //   直接走 _wfopen(UTF-16) + fread 完全绕开 ANSI 转换。
    std::string content;
#if defined(_WIN32)
    std::FILE* fp = ::_wfopen(path.wstring().c_str(), L"rb");
    if (!fp) {
        throw std::runtime_error("SimPointTable: cannot open file '" + path.generic_string() + "'");
    }
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
        content.append(buf, n);
    }
    std::fclose(fp);
#else
    std::ifstream is(path);
    if (!is.is_open()) {
        throw std::runtime_error("SimPointTable: cannot open file '" + path.generic_string() + "'");
    }
    std::ostringstream oss;
    oss << is.rdbuf();
    content = oss.str();
#endif

    json root;
    try {
        root = json::parse(content.begin(), content.end());
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("SimPointTable: JSON parse error in ")
                                 + path.generic_string() + ": " + e.what());
    }

    // schemaVersion 校验
    const auto& meta = root.value("meta", json::object());
    const std::string ver = meta.value("schemaVersion", "0.0");
    if (ver != "1.0" && ver != "1.1") {
        throw std::runtime_error("SimPointTable: unsupported schemaVersion '" + ver + "' (accept 1.0/1.1)");
    }

    auto tbl = std::make_shared<SimPointTable>();
    const auto& points = root.at("points");
    tbl->m_bySlave.reserve(/*meta 设备数 + slack*/ 32);

    for (const auto& jp : points) {
        SimPoint p;
        p.pointId      = jp.at("pointId").get<uint32_t>();
        p.pointName    = jp.value("pointName", "");
        p.slaveAddress = jp.at("slaveAddress").get<uint8_t>();
        p.regType      = parseRegisterType(jp.at("regType").get<std::string>());
        p.dataType     = parseDataType(jp.value("dataType", "Float32"));
        p.byteOrder    = parseByteOrder(jp.value("byteOrder", "ABCD"));
        // registerAddr 可能为 int(0x10) 或 string("0x10") — 容忍两种
        if (jp.at("registerAddr").is_string()) {
            p.registerAddr = parseHexOrDec(jp.at("registerAddr").get<std::string>());
        } else {
            p.registerAddr = jp.at("registerAddr").get<uint16_t>();
        }
        p.scaleFactor  = jp.value("scaleFactor", 1.0f);
        p.offset       = jp.value("offset",      0.0f);
        p.unit         = jp.value("unit",        "");
        p.enabled      = jp.value("enabled",     true);

        tbl->m_bySlave[p.slaveAddress].push_back(p);
    }

    // 反向 name 索引
    for (auto& [slave, vec] : tbl->m_bySlave) {
        (void)slave;
        for (const auto& p : vec) {
            if (!p.pointName.empty()) {
                tbl->m_byName[p.pointName] = &p;
            }
        }
    }

    return tbl;
}

const std::vector<SimPoint>& SimPointTable::onSlave(uint8_t slave) const noexcept {
    static const std::vector<SimPoint> kEmpty{};
    const auto it = m_bySlave.find(slave);
    return (it != m_bySlave.end()) ? it->second : kEmpty;
}

const SimPoint* SimPointTable::findByName(const std::string& name) const noexcept {
    const auto it = m_byName.find(name);
    return (it != m_byName.end()) ? it->second : nullptr;
}

size_t SimPointTable::pointCount() const noexcept {
    size_t n = 0;
    for (const auto& [_, v] : m_bySlave) n += v.size();
    return n;
}

std::vector<uint8_t> SimPointTable::allSlaveIds() const noexcept {
    std::vector<uint8_t> ids;
    ids.reserve(m_bySlave.size());
    for (const auto& [id, _] : m_bySlave) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool SimPointTable::hasHoldingAt(uint8_t slave, uint16_t addr) const noexcept {
    const auto& v = onSlave(slave);
    for (const auto& p : v) {
        if (p.regType == RegisterType::HoldingRegister && p.registerAddr == addr) return true;
    }
    return false;
}

}  // namespace ens::sim
