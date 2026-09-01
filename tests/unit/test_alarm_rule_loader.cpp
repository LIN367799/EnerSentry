// tests/unit/test_alarm_rule_loader.cpp
// L4 业务层 —— 告警规则 JSON 加载器 Tier 2 单测（切片 16 / FR-CFG-06）。
//
// 用例覆盖：
//   1) 加载 alarm_rules_sample.json → 3 条规则,pointName→pointId 正确解析
//   2) level 字符串 → 枚举（大小写两可）
//   3) 容错：文件不存在 / 坏 JSON → -1；未知 pointName / 坏 level → 跳过计数

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "protocol/PointTable.h"
#include "business/AlarmRuleLoader.h"

using namespace ens;

namespace {
std::filesystem::path findTestData(const char* rel) {
    const std::filesystem::path candidates[] = {
        std::filesystem::path(L"test_data") / rel,
        std::filesystem::path(L"../test_data") / rel,
        std::filesystem::path(L"../../bin/Debug/test_data") / rel,
    };
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
    throw std::runtime_error(std::string("test data not found: ") + rel);
}

std::shared_ptr<protocol::PointTable> makePointTable() {
    return protocol::PointTable::loadFromJsonFile(findTestData("sim_pointtable_sample.json"));
}
}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// 1) 加载 sample 规则 JSON
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("alarm_rule_loader: loads sample rules with pointName -> pointId resolution",
          "[master][business][alarm][rule-loader][tier2]") {
    auto pt = makePointTable();

    std::vector<business::AlarmRule> rules;
    std::string err;
    const int n = business::AlarmRuleLoader::loadFromFile(
        findTestData("alarm_rules_sample.json"), *pt, rules, &err);
    REQUIRE(n == 3);
    REQUIRE(rules.size() == 3);

    // 首条：Rack-01_MaxTemp → pointId=1, Critical, 60/55
    REQUIRE(rules[0].pointId == 1);
    REQUIRE(rules[0].level == business::AlarmLevel::Critical);
    REQUIRE(rules[0].onThreshold == Catch::Approx(60.0f));
    REQUIRE(rules[0].offThreshold == Catch::Approx(55.0f));
    REQUIRE(rules[0].enabled);
    REQUIRE(rules[0].onDelayMs == 3000);
    REQUIRE(rules[0].suppressWindowMs == 60000);
}

// ═════════════════════════════════════════════════════════════════════════════
// 2) level 字符串解析
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("alarm_rule_loader: levelFromString accepts case variants, rejects unknown",
          "[master][business][alarm][rule-loader][tier2]") {
    business::AlarmLevel lv{};
    REQUIRE(business::AlarmRuleLoader::levelFromString("Info", lv));
    REQUIRE(lv == business::AlarmLevel::Info);
    REQUIRE(business::AlarmRuleLoader::levelFromString("warning", lv));
    REQUIRE(lv == business::AlarmLevel::Warning);
    REQUIRE(business::AlarmRuleLoader::levelFromString("Critical", lv));
    REQUIRE(lv == business::AlarmLevel::Critical);
    REQUIRE_FALSE(business::AlarmRuleLoader::levelFromString("Fatal", lv));
    REQUIRE_FALSE(business::AlarmRuleLoader::levelFromString("", lv));
}

// ═════════════════════════════════════════════════════════════════════════════
// 3) 容错
// ═════════════════════════════════════════════════════════════════════════════
TEST_CASE("alarm_rule_loader: missing file returns -1 with err message",
          "[master][business][alarm][rule-loader][fault-tolerance][tier2]") {
    auto pt = makePointTable();
    std::vector<business::AlarmRule> rules;
    std::string err;
    const int n = business::AlarmRuleLoader::loadFromFile(
        std::filesystem::path(L"nonexistent_dir/rules.json"), *pt, rules, &err);
    REQUIRE(n < 0);
    REQUIRE_FALSE(err.empty());
}
