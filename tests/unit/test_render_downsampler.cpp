// tests/unit/test_render_downsampler.cpp —— RenderDownsampler 单测（切片 20）。
// 覆盖：原样返回分支 / 点数上限 / 尖峰保留 / 末端点连续 / 空输入。
// ⚠ TEST_CASE 第一参数严格 ASCII（项目测试铁律）。
#include <catch2/catch_test_macros.hpp>

#include "charts/RenderDownsampler.h"

using ens::ui::RenderDownsampler;

namespace {
QVector<QPointF> ramp(int n, double base) {
    QVector<QPointF> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) v.push_back(QPointF(double(i), base + i * 0.5));
    return v;
}
}  // namespace

TEST_CASE("render_downsampler: returns src unchanged when under target") {
    const auto src = ramp(10, 0.0);
    const auto out = RenderDownsampler::minMaxBucketDownSample(src, 20);
    REQUIRE(out.size() == 10);
    REQUIRE(out == src);
}

TEST_CASE("render_downsampler: empty input yields empty output") {
    const QVector<QPointF> empty;
    REQUIRE(RenderDownsampler::minMaxBucketDownSample(empty, 100).isEmpty());
}

TEST_CASE("render_downsampler: output size never exceeds target") {
    const auto src = ramp(5000, 0.0);
    const auto out = RenderDownsampler::minMaxBucketDownSample(src, 2000);
    REQUIRE(out.size() <= 2000);
    REQUIRE(out.size() >= 2);
}

TEST_CASE("render_downsampler: preserves spike (min/max bucket)") {
    // 构造含单个巨大尖峰的序列：尖峰值必须出现在输出中
    QVector<QPointF> src;
    src.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        src.push_back(QPointF(double(i), i == 500 ? 999.0 : double(i)));
    }
    const auto out = RenderDownsampler::minMaxBucketDownSample(src, 100);
    REQUIRE(out.size() <= 100);
    bool sawSpike = false;
    for (const auto& p : out) {
        if (p.y() == 999.0) { sawSpike = true; break; }
    }
    REQUIRE(sawSpike);
}

TEST_CASE("render_downsampler: last point preserved for x-axis continuity") {
    const auto src = ramp(5000, 0.0);
    const auto out = RenderDownsampler::minMaxBucketDownSample(src, 500);
    REQUIRE(out.size() <= 500);
    REQUIRE(out.last().x() == src.last().x());   // 右边界连续
}
