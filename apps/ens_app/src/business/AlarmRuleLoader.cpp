// src/business/AlarmRuleLoader.cpp —— 见 AlarmRuleLoader.h。
#include "AlarmRuleLoader.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>

namespace ens::business {

using json = nlohmann::json;

namespace {

// 读文件到 string：Windows 走 _wfopen(wide path) 绕开 std::ifstream 的 ANSI 中文路径坑
std::string readAllWide(const std::filesystem::path& p) {
#ifdef _WIN32
    FILE* f = _wfopen(p.c_str(), L"rb");
#else
    FILE* f = std::fopen(p.c_str(), "rb");
#endif
    if (f == nullptr) return {};
    std::string out;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

}  // namespace

bool AlarmRuleLoader::levelFromString(const std::string& s, AlarmLevel& out) noexcept {
    if (s == "Info" || s == "info")     { out = AlarmLevel::Info;     return true; }
    if (s == "Warning" || s == "warning") { out = AlarmLevel::Warning; return true; }
    if (s == "Critical" || s == "critical") { out = AlarmLevel::Critical; return true; }
    return false;
}

uint32_t AlarmRuleLoader::resolvePointId(const protocol::PointTable& pt,
                                         const std::string& name) noexcept {
    for (const auto* pr : pt.allPoints()) {
        if (pr != nullptr && pr->pointName == name) return pr->pointId;
    }
    return 0;   // 未找到
}

int AlarmRuleLoader::loadFromFile(const std::filesystem::path& jsonPath,
                                  const protocol::PointTable& pt,
                                  std::vector<AlarmRule>& out,
                                  std::string* err) {
    const std::string content = readAllWide(jsonPath);
    if (content.empty()) {
        if (err) *err = "open failed: " + jsonPath.generic_string();
        return -1;
    }
    const json doc = json::parse(content, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("rules") ||
        !doc["rules"].is_array()) {
        if (err) *err = "missing rules[]: " + jsonPath.generic_string();
        return -1;
    }

    int okCount = 0;
    std::string issues;
    for (const auto& j : doc["rules"]) {
        AlarmRule r;
        // pointName → pointId
        const std::string name = j.value("pointName", std::string());
        if (name.empty()) { issues += "empty pointName; "; continue; }
        r.pointId = resolvePointId(pt, name);
        if (r.pointId == 0) {
            issues += "point '" + name + "' not found in point table; ";
            continue;
        }
        // level 字符串 → 枚举
        const std::string lvl = j.value("level", std::string("Warning"));
        if (!levelFromString(lvl, r.level)) {
            issues += "bad level '" + lvl + "' for " + name + "; ";
            continue;
        }
        r.onThreshold         = static_cast<float>(j.value("onThreshold", 0.0));
        r.offThreshold        = static_cast<float>(j.value("offThreshold", 0.0));
        r.enabled             = j.value("enabled", true);
        r.onDelayMs           = j.value("onDelayMs", uint32_t{3000});
        r.offDelayMs          = j.value("offDelayMs", uint32_t{3000});
        r.suppressWindowMs    = j.value("suppressWindowMs", uint32_t{60000});
        out.push_back(r);
        ++okCount;
    }
    if (err) *err = issues;
    return okCount;
}

}  // namespace ens::business
