#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <QMetaType>

// BUILD-0 冒烟：验证 vcpkg 依赖（Catch2 / nlohmann_json / spdlog）
// 经 ens::3rdparty 正确注入，整条「编译器 + Qt + vcpkg + CMake 三自动」链路通了。
TEST_CASE("BUILD-0 sanity: dependencies wired", "[smoke]") {
    REQUIRE(true);

    nlohmann::json j = {{"app", "EnerSentry"}, {"build", "BUILD-0"}};
    REQUIRE(j["app"].get<std::string>() == "EnerSentry");

    spdlog::info("BUILD-0 sanity test ok");
}

TEST_CASE("qt metatypes: fixed-width integer aliases are registered", "[smoke][qt]") {
    REQUIRE(QMetaType::type("uint8_t") != QMetaType::UnknownType);
    REQUIRE(QMetaType::type("uint16_t") != QMetaType::UnknownType);
    REQUIRE(QMetaType::type("uint32_t") != QMetaType::UnknownType);
}
