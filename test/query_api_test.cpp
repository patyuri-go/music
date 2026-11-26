// /api/query 类型守卫回归测试（集成层，走真实 HTTP）。
//
// 覆盖：请求体里字段类型不符时，接口不得抛异常崩成 500。
//   - 数字字段写成字符串（"5"）、对象、极大值
//   - 字符串字段写成数组
//   - type 写成数组 → 应走 404（与未知 type 同路径），不是 500
//
// 需要活的 musicd 与数据库，由 test/run_integration.sh 驱动：
//   MUSICD_HOST=127.0.0.1 MUSICD_PORT=15667 build/bin/test/query_api_test
// 不在 CTest 里注册（ctest 必须保持纯单元、无库无网）。

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "json_util.h"
#include "test_http.h"

namespace {

std::string host() {
  const char *value = std::getenv("MUSICD_HOST");
  return (value == nullptr || *value == '\0') ? "127.0.0.1" : std::string(value);
}

int port() {
  const char *value = std::getenv("MUSICD_PORT");
  return (value == nullptr || *value == '\0') ? 5667 : std::atoi(value);
}

test_http::Response postQuery(const std::string &body) {
  return test_http::httpRequest(host(), port(), "POST", "/api/query", body);
}

// 成功响应必须是 JSON 数组。返回 false 时调用方拿到失败信息。
void expectOkArray(const test_http::Response &response,
                   const std::string &case_name) {
  SCOPED_TRACE(case_name);
  ASSERT_EQ(response.status, 200) << "body=" << response.body;
  const Json::Value json = json_util::parseOrNull(response.body);
  EXPECT_FALSE(json.isNull()) << "body=" << response.body;
  EXPECT_TRUE(json.isArray()) << "body=" << response.body;
}

} // namespace

// 对照组：不带可选参数时照常返回。
TEST(QueryApiTypeGuardTest, HomeWithoutParams) {
  expectOkArray(postQuery(R"({"type":"home"})"), "{\"type\":\"home\"}");
}

// 数字字段写成字符串：修复前 jsoncpp 的 asInt64() 直接抛异常 -> 500。
TEST(QueryApiTypeGuardTest, HomeWithStringAfterId) {
  expectOkArray(postQuery(R"({"type":"home","afterid":"5"})"),
                "afterid=\"5\"");
}

TEST(QueryApiTypeGuardTest, HomeWithObjectAfterId) {
  expectOkArray(postQuery(R"({"type":"home","afterid":{}})"),
                "afterid={}");
}

TEST(QueryApiTypeGuardTest, HomeWithStringPegcountZero) {
  expectOkArray(postQuery(R"({"type":"home","pegcount":"0"})"),
                "pegcount=\"0\"");
}

// 上限钳制在集成层不可观测（夹具曲目数远小于 page_size），这里只证明不崩。
TEST(QueryApiTypeGuardTest, HomeWithHugePegcount) {
  expectOkArray(postQuery(R"({"type":"home","pegcount":"999999"})"),
                "pegcount=\"999999\"");
}

TEST(QueryApiTypeGuardTest, SearchWithArrayQuery) {
  expectOkArray(postQuery(R"({"type":"search","q":[]})"), "q=[]");
}

TEST(QueryApiTypeGuardTest, TagWithArrayTag) {
  expectOkArray(postQuery(R"({"type":"tag","tag":[]})"), "tag=[]");
}

// type 不是字符串时按无效请求处理，走现有「未知 type」的 404 路径。
TEST(QueryApiTypeGuardTest, NonStringTypeReturns404) {
  const test_http::Response response = postQuery(R"({"type":[]})");
  EXPECT_EQ(response.status, 404) << "body=" << response.body;
}

// 对照组：负数 afterid 与不传等价（内部 id 从 1 起）。
TEST(QueryApiTypeGuardTest, HomeWithNegativeAfterId) {
  expectOkArray(postQuery(R"({"type":"home","afterid":-5})"),
                "afterid=-5");
}