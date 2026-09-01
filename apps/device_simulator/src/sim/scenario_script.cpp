// src/sim/scenario_script.cpp
// 场景脚本驱动实现 —— 见 scenario_script.h（DevGuide §4B B9 切片 15）。
#include "sim/scenario_script.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

namespace ens::sim {

using json = nlohmann::json;

namespace {

// 宽松字符串解析：stoi 失败/空 → 默认值
int64_t toInt64(const std::string& s, int64_t def) noexcept {
    if (s.empty()) return def;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    return (end != s.c_str()) ? static_cast<int64_t>(v) : def;
}

// 读文件到 string：Windows 走 _wfopen(wide path) 绕开 std::ifstream 的 ANSI code page
// 中文路径坑（Phase 2 3.1.5 实测坑 #6：std::filesystem + MSVC + 中文路径三重坑）。
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

// ISO8601 UTC 时间戳（毫秒精度补 000）—— SIM-IMP §8.1 ts 字段
std::string isoNow() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &tt);
#else
    gmtime_r(&tt, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
    return std::string(buf) + ".000Z";
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// load / reset
// ─────────────────────────────────────────────────────────────────────────────

bool ScenarioScript::load(const std::string& path) {
    m_steps.clear();
    m_name.clear();
    m_report = ScenarioReport{};
    m_events.clear();
    m_handles.clear();
    m_started = false;
    m_finished = false;
    m_anyFail = false;

    try {
        const std::string content = readAllWide(std::filesystem::path(path));
        if (content.empty()) {
            m_report.notes = "open failed: " + path;
            return false;
        }
        const json doc = json::parse(content, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded() || !doc.is_object()) {
            m_report.notes = "parse failed (invalid JSON): " + path;
            return false;
        }
        m_name = doc.value("name", std::string());
        if (!doc.contains("steps") || !doc["steps"].is_array()) {
            m_report.notes = "missing steps[]: " + path;
            return false;
        }
        for (const auto& j : doc["steps"]) {
            ScenarioStep s;
            s.tMs        = j.value("t", static_cast<int64_t>(0));
            s.action     = j.value("action", std::string());
            s.fault      = j.value("fault", std::string());
            s.scope      = j.value("scope", std::string());
            s.regName    = j.value("register", std::string());
            s.count      = j.value("count", 1);
            s.targetValue= static_cast<float>(j.value("targetValue", 0.0));
            s.rampRate   = static_cast<float>(j.value("rampRate", 0.0));
            s.durationMs = j.value("durationMs", static_cast<int64_t>(0));
            s.alarmBit   = j.value("alarmBit", -1);
            // slave: "ALL" 或 int
            if (j.contains("slave")) {
                const auto& sl = j["slave"];
                if (sl.is_string()) s.slaveSpec = sl.get<std::string>();
                else if (sl.is_number_integer()) s.slaveSpec = std::to_string(sl.get<int>());
            }
            if (s.action != "INJECT" && s.action != "RECOVER") {
                m_anyFail = true;
                m_report.notes += "bad action '" + s.action + "'; ";
                continue;
            }
            m_steps.push_back(std::move(s));
        }
        // 按 t 升序（drive 依赖有序触发；输入已有序，防御性排序）
        std::stable_sort(m_steps.begin(), m_steps.end(),
                         [](const ScenarioStep& a, const ScenarioStep& b) { return a.tMs < b.tMs; });
    } catch (const std::exception& e) {
        m_report.notes = std::string("exception: ") + e.what();
        return false;
    }
    return !m_steps.empty() || !m_anyFail;
}

void ScenarioScript::reset() {
    for (auto& s : m_steps) s.fired = false;
    m_handles.clear();
    m_events.clear();
    m_started = false;
    m_finished = false;
    m_anyFail = false;
    m_report = ScenarioReport{};
}

// ─────────────────────────────────────────────────────────────────────────────
// 查询
// ─────────────────────────────────────────────────────────────────────────────

int64_t ScenarioScript::lastStepEndMs() const noexcept {
    int64_t end = 0;
    for (const auto& s : m_steps) {
        end = std::max(end, s.tMs + s.durationMs);
    }
    return end;
}

bool ScenarioScript::allFired() const noexcept {
    return std::all_of(m_steps.begin(), m_steps.end(),
                       [](const ScenarioStep& s) { return s.fired; });
}

// ─────────────────────────────────────────────────────────────────────────────
// 时钟驱动
// ─────────────────────────────────────────────────────────────────────────────

int ScenarioScript::drive(int64_t nowMs, FaultInjector& fi, const SimPointTable& pt) {
    if (!m_started) {
        m_started = true;
        m_startMs = nowMs;
    }
    int fired = 0;
    for (auto& s : m_steps) {
        if (s.fired || s.tMs > nowMs) continue;
        s.fired = true;
        if (s.action == "INJECT") {
            fireInject(s, fi, pt);
        } else if (s.action == "RECOVER") {
            fireRecover(s, fi);
        }
        ++fired;
    }
    return fired;
}

// ─────────────────────────────────────────────────────────────────────────────
// INJECT / RECOVER 展开
// ─────────────────────────────────────────────────────────────────────────────

std::optional<FaultType> ScenarioScript::faultTypeFromCode(const std::string& code) noexcept {
    if (code == "FR-SIM-05a") return FaultType::OverTemp;
    if (code == "FR-SIM-05b") return FaultType::CellVoltage;
    if (code == "FR-SIM-05c") return FaultType::CommLoss;
    if (code == "FR-SIM-05d") return FaultType::CrcError;
    if (code == "FR-SIM-05e") return FaultType::Timeout;
    return std::nullopt;
}

const SimPoint* ScenarioScript::findPointBySuffix(const SimPointTable& pt, uint8_t slave,
                                                  const std::string& regName, int idx) noexcept {
    const auto& v = pt.onSlave(slave);
    // cell 展开：优先 "_<regName>_%03d"，再 "_<regName>_<idx>"（全量/样例命名兼容）
    if (idx >= 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "_%s_%03d", regName.c_str(), idx);
        const std::string pat3(buf);
        std::snprintf(buf, sizeof(buf), "_%s_%d", regName.c_str(), idx);
        const std::string patPlain(buf);
        for (const auto& p : v) {
            if (!p.enabled) continue;
            if (p.pointName.size() >= pat3.size() &&
                p.pointName.compare(p.pointName.size() - pat3.size(), pat3.size(), pat3) == 0)
                return &p;
            if (p.pointName.size() >= patPlain.size() &&
                p.pointName.compare(p.pointName.size() - patPlain.size(), patPlain.size(), patPlain) == 0)
                return &p;
        }
        return nullptr;
    }
    // 单寄存器：后缀 "_<regName>"
    const std::string pat = "_" + regName;
    for (const auto& p : v) {
        if (!p.enabled) continue;
        if (p.pointName.size() >= pat.size() &&
            p.pointName.compare(p.pointName.size() - pat.size(), pat.size(), pat) == 0)
            return &p;
    }
    return nullptr;
}

std::string ScenarioScript::handleKey(const ScenarioStep& s) const noexcept {
    return s.fault + "|" + s.scope + "|" + s.slaveSpec + "|" + s.regName;
}

void ScenarioScript::fireInject(const ScenarioStep& s, FaultInjector& fi,
                                const SimPointTable& pt) {
    const auto typeOpt = faultTypeFromCode(s.fault);
    if (!typeOpt.has_value()) {
        m_anyFail = true;
        m_report.notes += "unknown fault code '" + s.fault + "'; ";
        appendEvent("ERROR", "FAULT_INJECT_UNKNOWN", 0, 0, s.fault, "unknown fault code");
        return;
    }
    const FaultType type = *typeOpt;

    // 展开 slave 列表
    const bool all = (s.slaveSpec == "ALL");
    std::vector<uint8_t> slaves;
    if (all) {
        slaves = pt.allSlaveIds();
    } else {
        const int64_t sid = toInt64(s.slaveSpec, 0);
        if (sid <= 0 || sid > 255) {
            m_anyFail = true;
            m_report.notes += "bad slave '" + s.slaveSpec + "'; ";
            return;
        }
        slaves.push_back(static_cast<uint8_t>(sid));
    }

    int injected = 0;
    const auto pushReq = [&](uint8_t slave, uint16_t reg, Scope sc) {
        FaultRequest req;
        req.spec.type       = type;
        req.spec.scope      = sc;
        req.spec.slave      = slave;
        req.spec.reg        = reg;
        req.spec.targetValue = s.targetValue;
        req.spec.rampRate   = s.rampRate;
        req.spec.durationMs = static_cast<int32_t>(s.durationMs);
        const FaultHandle h = fi.trigger(req);
        if (h == INVALID_FAULT_HANDLE) {
            m_anyFail = true;
            m_report.notes += "trigger failed slave=" + std::to_string(slave) + "; ";
            return;
        }
        m_handles[handleKey(s)].push_back(h);
        ++injected;
        appendEvent("WARN", "FAULT_INJECT", slave, reg, s.fault,
                    std::string("target=") + std::to_string(s.targetValue) + " ramp=" +
                        std::to_string(s.rampRate) + (s.alarmBit >= 0
                            ? " alarmBit=" + std::to_string(s.alarmBit) : ""));
    };

    if (type == FaultType::CommLoss || s.scope == "pcs" || s.scope == "link") {
        // 链路级故障：SLAVE scope（B8 教训——POINT 精确 key 不命中 linkEffect 的 SLAVE/ALL 通配）
        for (uint8_t slave : slaves) {
            pushReq(slave, 0, Scope::SLAVE);
        }
    } else if (s.regName.empty()) {
        m_anyFail = true;
        m_report.notes += "register required for scope '" + s.scope + "'; ";
        return;
    } else if (s.scope == "cell" && s.count > 1) {
        // 单体展开：Rack-XX_CellV_000..0<count-1>
        for (uint8_t slave : slaves) {
            for (int i = 0; i < s.count; ++i) {
                const SimPoint* p = findPointBySuffix(pt, slave, s.regName, i);
                if (p == nullptr) {
                    m_anyFail = true;
                    m_report.notes += "point '" + s.regName + "_" + std::to_string(i) +
                                      "' not found on slave " + std::to_string(slave) + "; ";
                    continue;
                }
                pushReq(slave, p->registerAddr, Scope::POINT);
            }
        }
    } else {
        // 单寄存器 / 簇级：Rack-XX_<regName>
        for (uint8_t slave : slaves) {
            const SimPoint* p = findPointBySuffix(pt, slave, s.regName, -1);
            if (p == nullptr) {
                // 该 slave 无此寄存器（如样例点表 Rack-16 无 MaxTemp）→ 跳过不报错
                continue;
            }
            pushReq(slave, p->registerAddr, Scope::POINT);
        }
    }

    // 汇总到 report（fault -> count）
    auto it = std::find_if(m_report.faultsInjected.begin(), m_report.faultsInjected.end(),
                           [&](const std::pair<std::string, int>& e) { return e.first == s.fault; });
    if (it != m_report.faultsInjected.end()) it->second += injected;
    else if (injected > 0) m_report.faultsInjected.emplace_back(s.fault, injected);
}

void ScenarioScript::fireRecover(const ScenarioStep& s, FaultInjector& fi) {
    const std::string key = handleKey(s);
    auto it = m_handles.find(key);
    if (it == m_handles.end() || it->second.empty()) {
        // 无对应 INJECT → 记录但不判 FAIL（recover 幂等语义，恢复指令允许先于/缺触发）
        appendEvent("INFO", "FAULT_RECOVER_SKIP", 0, 0, s.fault, "no matching inject handle");
        return;
    }
    const FaultHandle h = it->second.front();
    it->second.erase(it->second.begin());
    // 回归目标 = RECOVER step 的 targetValue（overheat: 65→35℃；voltage: 3.65→3.30V）
    if (!fi.recover(h, s.targetValue)) {
        m_anyFail = true;
        m_report.notes += "recover failed handle=" + std::to_string(h) + "; ";
    }
    appendEvent("INFO", "FAULT_RECOVER", 0, 0, s.fault, "recovering handle=" + std::to_string(h));
}

// ─────────────────────────────────────────────────────────────────────────────
// 事件流 / 报告
// ─────────────────────────────────────────────────────────────────────────────

void ScenarioScript::appendEvent(const char* level, const char* event, uint8_t slave,
                                 uint16_t reg, const std::string& fault,
                                 const std::string& detail) {
    json j;
    j["ts"]      = isoNow();
    j["level"]   = level;
    j["event"]   = event;
    j["slave"]   = slave;
    j["register"]= reg;
    j["fault"]   = fault.empty() ? json(nullptr) : json(fault);
    j["detail"]  = detail;
    m_events.push_back(j.dump());
}

void ScenarioScript::finishReport(int64_t durationMs, bool aborted) {
    m_finished = true;
    m_report.scenarioName = m_name;
    m_report.durationMs   = durationMs;
    if (aborted) {
        m_report.result = "INCONCLUSIVE";
        m_report.notes = (m_report.notes.empty() ? std::string() : m_report.notes + "; ") +
                         std::string("scenario aborted before completion");
    } else if (m_anyFail) {
        m_report.result = "FAIL";
    } else if (allFired()) {
        m_report.result = "PASS";
    } else {
        m_report.result = "INCONCLUSIVE";
        m_report.notes = (m_report.notes.empty() ? std::string() : m_report.notes + "; ") +
                         std::string("not all steps fired");
    }
}

std::string ScenarioScript::reportJson() const {
    json j;
    j["scenario"] = m_report.scenarioName;
    j["durationMs"] = m_report.durationMs;
    json arr = json::array();
    for (const auto& [code, count] : m_report.faultsInjected) {
        arr.push_back({{"fault", code}, {"count", count}});
    }
    j["faultsInjected"] = arr;
    j["badFramesDropped"] = m_report.badFramesDropped;
    j["linkLossEvents"]   = m_report.linkLossEvents;
    j["result"]  = m_report.result;
    j["notes"]   = m_report.notes;
    return j.dump(2);
}

std::string ScenarioScript::eventsJsonl() const {
    std::string out;
    for (const auto& e : m_events) {
        out += e;
        out += '\n';
    }
    return out;
}

}  // namespace ens::sim
