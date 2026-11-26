// 歌词接口测试（夹具驱动）：track_lyrics 行直接决定 Jellyfin Lyrics 输出。
//
// 用法：build/bin/test/lyrics_test [host] [port] [jellyfin_port]
//       （默认 127.0.0.1:5667/9000）
// 依赖 test_fixture seed 导出的环境变量。

#include "test_http.h"

#include "fixture_ids.h"

#include <cstdlib>
#include <iostream>
#include <string>

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

} // namespace

int main(int argc, char **argv) {
  const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
  const int port = argc > 2 ? std::atoi(argv[2]) : 5667;
  const int jf_port = argc > 3 ? std::atoi(argv[3]) : 9000;

  const std::string role_ref = fixtureEnv(fixture::kEnvTrackRoleRef);
  const std::string plain_ref = fixtureEnv(fixture::kEnvTrackPlainRef);

  std::cout << "== musicd JSON 歌词 ==\n";
  {
    const test_http::Response r =
        test_http::httpRequest(host, port, "GET",
                               "/api/track/" + role_ref + "/lyrics");
    check(r.status == 200, "GET /api/track/<ref>/lyrics -> 200");
    const auto json = json_util::parseOrNull(r.body);
    check(!json.isNull() && json.isMember("synced") &&
              json["synced"].asBool(),
          "synced=true");
    const std::string text =
        json.isNull() || !json.isMember("text") ? std::string()
                                                : json["text"].asString();
    check(text.find("第一行") != std::string::npos &&
              text.find("第二行") != std::string::npos,
          "TTML 文本包含两行歌词");
  }
  {
    const test_http::Response r =
        test_http::httpRequest(host, port, "GET",
                               "/api/track/" + plain_ref + "/lyrics");
    const auto json = json_util::parseOrNull(r.body);
    const bool empty = json.isNull() || !json.isMember("synced") ||
                       !json["synced"].asBool();
    check(r.status == 200 && empty, "无歌词曲目 synced=false");
  }

  std::cout << "\n== Jellyfin LyricDto ==\n";
  {
    const test_http::Response r = test_http::httpRequest(
        host, jf_port, "GET", "/Audio/" + role_ref + "/Lyrics");
    check(r.status == 200, "GET /Audio/<guid>/Lyrics -> 200");
    const auto json = json_util::parseOrNull(r.body);
    check(!json.isNull() && json.isMember("Lyrics") &&
              json["Lyrics"].isArray() && json["Lyrics"].size() >= 2,
          "Lyrics 至少 2 行");
    check(!json.isNull() && json.isMember("Metadata") &&
              json["Metadata"].isMember("IsSynced") &&
              json["Metadata"]["IsSynced"].asBool(),
          "Metadata.IsSynced=true");
    if (!json.isNull() && json.isMember("Lyrics") &&
        json["Lyrics"].isArray() && json["Lyrics"].size() > 0) {
      check(json["Lyrics"][0].isMember("Start") &&
                json["Lyrics"][0]["Start"].asInt64() == 15000000,
            "首行 Start=15000000");
      check(json["Lyrics"][0].isMember("Text") &&
                json["Lyrics"][0]["Text"].asString() == "第一行",
            "首行 Text=第一行");
    }
  }
  {
    const test_http::Response r = test_http::httpRequest(
        host, jf_port, "GET", "/Items/" + role_ref);
    check(r.status == 200, "GET /Items/<guid> -> 200");
    const auto json = json_util::parseOrNull(r.body);
    check(!json.isNull() && json.isMember("HasLyrics") &&
              json["HasLyrics"].asBool(),
          "HasLyrics=true");
  }

  std::cout << "\n== 无歌词曲目 ==\n";
  {
    const test_http::Response r = test_http::httpRequest(
        host, jf_port, "GET", "/Audio/" + plain_ref + "/Lyrics");
    check(r.status == 404, "无歌词 -> 404");
    const test_http::Response item = test_http::httpRequest(
        host, jf_port, "GET", "/Items/" + plain_ref);
    const auto json = json_util::parseOrNull(item.body);
    check(json.isNull() || !json.isMember("HasLyrics") ||
              !json["HasLyrics"].asBool(),
          "无歌词曲目 HasLyrics=false");
  }

  std::cout << "\n结果: PASS=" << g_pass << " FAIL=" << g_fail << "\n";
  return g_fail == 0 ? 0 : 1;
}