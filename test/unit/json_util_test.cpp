// json_util::int_or / string_or 的类型边界测试。
//
// 这两个函数是 HTTP 请求体的取值入口：字段类型不符不该让整个请求崩掉。
// 本文件钉住的语义：
//   - 字符串形态的整数按数值解析（"42" -> 42），但拒绝尾部垃圾、前导空白、溢出；
//   - 只有 JSON string 才算字符串，数字/布尔/数组/对象/null 一律回退；
//   - 键缺失回退默认值。
//
// 纯函数，不读配置、不连库、不出网。

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "json_util.h"

namespace {

// fallback 故意用非零值，才能区分「回退成功」和「恰好解析成 0」。
constexpr std::int64_t kIntFallback = 7;
constexpr const char *kStringFallback = "fb";

std::int64_t intOf(const char *json_text, const char *key) {
  return json_util::int_or(json_util::parseOrNull(json_text), key, kIntFallback);
}

std::string stringOf(const char *json_text, const char *key) {
  return json_util::string_or(json_util::parseOrNull(json_text), key,
                              kStringFallback);
}

} // namespace

// ---------------------------------------------------------------------------
// int_or
// ---------------------------------------------------------------------------

TEST(JsonIntOrTest, KeepsNumericValue) {
  EXPECT_EQ(intOf(R"({"v":42})", "v"), 42);
}

TEST(JsonIntOrTest, ParsesNumericString) {
  EXPECT_EQ(intOf(R"({"v":"42"})", "v"), 42);
}

TEST(JsonIntOrTest, ParsesNegativeString) {
  EXPECT_EQ(intOf(R"({"v":"-5"})", "v"), -5);
}

TEST(JsonIntOrTest, RejectsTrailingGarbage) {
  // stoll 会静默返回 12；要求整体可解析，所以必须回退。
  EXPECT_EQ(intOf(R"({"v":"12abc"})", "v"), kIntFallback);
}

TEST(JsonIntOrTest, RejectsEmptyString) {
  EXPECT_EQ(intOf(R"({"v":""})", "v"), kIntFallback);
}

TEST(JsonIntOrTest, RejectsNonNumericString) {
  EXPECT_EQ(intOf(R"({"v":"abc"})", "v"), kIntFallback);
}

TEST(JsonIntOrTest, RejectsLeadingSpace) {
  // from_chars 不跳过空白，与 client.cpp 的 parse_int64 同口径。
  EXPECT_EQ(intOf(R"({"v":" 5"})", "v"), kIntFallback);
}

TEST(JsonIntOrTest, RejectsOverflow) {
  EXPECT_EQ(intOf(R"({"v":"99999999999999999999"})", "v"), kIntFallback);
}

TEST(JsonIntOrTest, RejectsMissingKey) {
  EXPECT_EQ(intOf(R"({"other":1})", "v"), kIntFallback);
}

TEST(JsonIntOrTest, RejectsNull) {
  EXPECT_EQ(intOf(R"({"v":null})", "v"), kIntFallback);
}

TEST(JsonIntOrTest, RejectsArrayAndObject) {
  EXPECT_EQ(intOf(R"({"v":[]})", "v"), kIntFallback);
  EXPECT_EQ(intOf(R"({"v":{}})", "v"), kIntFallback);
}

TEST(JsonIntOrTest, RejectsNonObjectRoot) {
  EXPECT_EQ(intOf(R"([1,2,3])", "v"), kIntFallback);
}

// ---------------------------------------------------------------------------
// string_or
// ---------------------------------------------------------------------------

TEST(JsonStringOrTest, KeepsString) {
  EXPECT_EQ(stringOf(R"({"v":"abc"})", "v"), "abc");
}

TEST(JsonStringOrTest, RejectsNumber) {
  // 不做 asString() 的隐式转换："42" 不是字符串。
  EXPECT_EQ(stringOf(R"({"v":42})", "v"), kStringFallback);
}

TEST(JsonStringOrTest, RejectsBool) {
  EXPECT_EQ(stringOf(R"({"v":true})", "v"), kStringFallback);
}

TEST(JsonStringOrTest, RejectsArrayAndObject) {
  EXPECT_EQ(stringOf(R"({"v":[]})", "v"), kStringFallback);
  EXPECT_EQ(stringOf(R"({"v":{}})", "v"), kStringFallback);
}

TEST(JsonStringOrTest, RejectsNull) {
  EXPECT_EQ(stringOf(R"({"v":null})", "v"), kStringFallback);
}

TEST(JsonStringOrTest, UsesFallbackForMissingKey) {
  EXPECT_EQ(stringOf(R"({"other":"x"})", "v"), kStringFallback);
}