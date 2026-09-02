// tests/unit/test_csv_writer.cpp —— CsvWriter Tier 2 单测（切片 34，FR-EXP-01 基建）。
// 覆盖：① UTF-8 BOM + 表头 + 普通行（逐字节验证）
//       ② RFC 4180 转义：逗号/双引号/换行字段 → 整体包裹 + " 翻倍
//       ③ UTF-8 多字节（中文/单位符号）原样保留
//       ④ 空字段/空格保留（连续逗号语义）
//       ⑤ open 失败路径：目录不存在 / 重复 open 拒绝

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <QTemporaryDir>

#include "ui/common/CsvWriter.h"

using ens::ui::CsvWriter;

namespace {

/// 读回整个文件为原始字节串（验证 BOM 时须看二进制）
std::string readFileBinary(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.is_open());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::filesystem::path tempFile(QTemporaryDir& dir, const char* name) {
    return std::filesystem::path(dir.path().toStdWString()) / name;
}

}  // namespace

TEST_CASE("csv writer: bom plus header plus plain rows", "[csv]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto p = tempFile(dir, "plain.csv");

    {
        CsvWriter w(p);
        REQUIRE(w.open());
        REQUIRE(w.writeRow({"time", "point", "value", "unit"}));
        REQUIRE(w.writeRow({"2026-09-02 10:00:00.000", "42", "34.2", "degC"}));
        w.close();
        REQUIRE_FALSE(w.isOpen());
    }

    const std::string content = readFileBinary(p);
    // UTF-8 BOM 三字节前缀
    REQUIRE(content.size() >= 3);
    REQUIRE(static_cast<uint8_t>(content[0]) == 0xEF);
    REQUIRE(static_cast<uint8_t>(content[1]) == 0xBB);
    REQUIRE(static_cast<uint8_t>(content[2]) == 0xBF);
    // 表头行 + 数据行（CRLF）
    REQUIRE(content.find("time,point,value,unit\r\n") != std::string::npos);
    REQUIRE(content.find("2026-09-02 10:00:00.000,42,34.2,degC\r\n") != std::string::npos);
}

TEST_CASE("csv writer: rfc4180 field escaping", "[csv]") {
    // 静态转义：含分隔符/引号/换行的字段必须整体包裹，内部 " 翻倍
    REQUIRE(CsvWriter::escapeField("plain") == "plain");
    REQUIRE(CsvWriter::escapeField("a,b") == "\"a,b\"");
    REQUIRE(CsvWriter::escapeField("say \"hi\"") == "\"say \"\"hi\"\"\"");
    REQUIRE(CsvWriter::escapeField("line1\nline2") == "\"line1\nline2\"");
    REQUIRE(CsvWriter::escapeField("cr\rlf") == "\"cr\rlf\"");

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto p = tempFile(dir, "esc.csv");

    {
        CsvWriter w(p);
        REQUIRE(w.open());
        REQUIRE(w.writeRow({"a,b", "say \"hi\"", "multi\nline"}));
        w.close();
    }
    const std::string content = readFileBinary(p);
    REQUIRE(content.find("\"a,b\",\"say \"\"hi\"\"\",\"multi\nline\"\r\n") != std::string::npos);
}

TEST_CASE("csv writer: utf8 multibyte preserved byte-exact", "[csv]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto p = tempFile(dir, "utf8.csv");

    const std::string chinese = "\xe6\xb5\x8b\xe7\x82\xb9";        // 测点（UTF-8）
    const std::string degC    = "\xe2\x84\x83";                     // ℃（UTF-8 三字节）
    {
        CsvWriter w(p);
        REQUIRE(w.open());
        REQUIRE(w.writeRow({chinese, degC}));
        w.close();
    }
    const std::string content = readFileBinary(p);
    REQUIRE(content.find(chinese + "," + degC + "\r\n") != std::string::npos);
    // 不得破坏 UTF-8 多字节序列（无 BOM 中间出现）
    REQUIRE(content.find(chinese) != std::string::npos);
}

TEST_CASE("csv writer: empty fields preserve empty columns", "[csv]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto p = tempFile(dir, "empty.csv");

    {
        CsvWriter w(p);
        REQUIRE(w.open());
        REQUIRE(w.writeRow({"a", "", "c"}));     // 中列空 → 连续逗号
        REQUIRE(w.writeRow({"", "", ""}));       // 全空行
        w.close();
    }
    const std::string content = readFileBinary(p);
    REQUIRE(content.find("a,,c\r\n") != std::string::npos);
    REQUIRE(content.find(",,\r\n") != std::string::npos);    // 全空行 → 空列由连续逗号表达
}

TEST_CASE("csv writer: open failure and reopen rejected", "[csv]") {
    // 目录不存在 → open 失败
    const auto missingDir =
        std::filesystem::path(QTemporaryDir().path().toStdWString()) / "no_such_sub" / "x.csv";
    {
        CsvWriter w(missingDir);
        REQUIRE_FALSE(w.open());
        REQUIRE_FALSE(w.isOpen());
    }

    // 重复 open 拒绝 + close 后再 open 允许（覆盖写）
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto p = tempFile(dir, "reopen.csv");
    CsvWriter w(p);
    REQUIRE(w.open());
    REQUIRE_FALSE(w.open());            // 已打开拒绝
    REQUIRE(w.writeRow({"first"}));
    w.close();
    REQUIRE(w.open());                  // 关闭后可重开（trunc 覆盖）
    REQUIRE(w.writeRow({"second"}));
    w.close();
    const std::string content = readFileBinary(p);
    REQUIRE(content.find("first") == std::string::npos);    // 已被 trunc
    REQUIRE(content.find("second") != std::string::npos);
}
