// 艺术家关系测试（夹具驱动）：曲目通过 track_artist 关联多个艺术家。
//
// 用法：build/bin/test/artists_test [host] [port]（默认 127.0.0.1:5667）
// 依赖 test_fixture seed 导出的环境变量。

#include "test_http.h"

#include "fixture_ids.h"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <json/json.h>

#include "json_util.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string &msg) {
  if (ok) {
    ++g_pass;
    std::cout << "  PASS  " << msg << "\n";
  } else {
    ++g_fail;
    std::cout << "  FAIL  " << msg << "\n";
  }
}

std::string fixtureEnv(const char *key) {
  const char *value = std::getenv(key);
  if (value == nullptr || *value == '\0') {
    std::cerr << "缺少环境变量 " << key << "（先运行 test_fixture seed）\n";
    std::exit(2);
  }
  return std::string{value};
}

// 读取 /api/track/<ref> 的 artists 数组，返回名字集合。
std::set<std::string> artistNames(const std::string &host, int port,
                                  const std::string &ref, int &count) {
  std::set<std::string> names;
  count = 0;
  const test_http::Response r = test_http::httpRequest(
      host, port, "GET", "/api/track/" + ref);
  check(r.status == 200, "GET /api/track/" + ref + " -> 200");
  auto json = json_util::parseOrNull(r.body);
  if (json.isNull() || !json.isMember("artists") ||
      !json["artists"].isArray()) {
    return names;
  }
  for (const auto &item : json["artists"]) {
    if (item.isMember("name")) {
      names.insert(item["name"].asString());
    }
  }
  count = static_cast<int>(json["artists"].size());
  return names;
}

} // namespace

int main(int argc, char **argv) {
  const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
  const int port = argc > 2 ? std::atoi(argv[2]) : 5667;

  const std::string role_ref = fixtureEnv(fixture::kEnvTrackRoleRef);
  const std::string plain_ref = fixtureEnv(fixture::kEnvTrackPlainRef);

  std::cout << "== 角色串拆出的 4 个艺术家 ==\n";
  int role_count = 0;
  const std::set<std::string> role_names =
      artistNames(host, port, role_ref, role_count);
  check(role_count == 4, "track.artists 数量 = 4（实际 " +
                             std::to_string(role_count) + "）");
  for (const auto &name : fixture::kRoleArtists) {
    check(role_names.count(name) == 1, "包含艺术家 " + name);
  }

  std::cout << "\n== 普通多值艺术家 ==\n";
  int plain_count = 0;
  const std::set<std::string> plain_names =
      artistNames(host, port, plain_ref, plain_count);
  check(plain_count == 2, "track.artists 数量 = 2（实际 " +
                              std::to_string(plain_count) + "）");
  for (const auto &name : fixture::kPlainArtists) {
    check(plain_names.count(name) == 1, "包含艺术家 " + name);
  }

  std::cout << "\n== 艺术家详情 ==\n";
  {
    const test_http::Response r =
        test_http::httpRequest(host, port, "GET", "/api/track/" + role_ref);
    auto json = json_util::parseOrNull(r.body);
    if (!json.isNull() && json.isMember("artists") &&
        json["artists"].isArray() && json["artists"].size() > 0) {
      const std::string id = json["artists"][0]["id"].asString();
      const test_http::Response detail = test_http::httpRequest(
          host, port, "GET", "/api/artist/" + id);
      check(detail.status == 200, "GET /api/artist/<id> -> 200");
      auto artist = json_util::parseOrNull(detail.body);
      check(!artist.isNull() && artist.isMember("name") &&
                role_names.count(artist["name"].asString()) == 1,
            "艺术家详情名字与关联一致");
    } else {
      check(false, "track.artists 非空");
    }
  }

  std::cout << "\n结果: PASS=" << g_pass << " FAIL=" << g_fail << "\n";
  return g_fail == 0 ? 0 : 1;
}