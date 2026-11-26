// 播放列表接口测试（夹具驱动，走真实 HTTP）。
//
// 用法：build/bin/test/playlist_test [host] [port] [jellyfin_port]
//       默认 127.0.0.1:5667 / :9000
//
// 退出码：0 表示全部通过，非 0 表示有失败。
// 依赖 test_fixture seed 导出的 FIXTURE_TRACK_ROLE_ID / FIXTURE_ALBUM_ID。

#include "test_http.h"

#include "fixture_ids.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include <json/json.h>

#include "json_util.h"

namespace {

using test_http::Response;

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

bool has(const Json::Value &j, const char *key) {
  return j.isObject() && j.isMember(key);
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

  const int track_id =
      std::atoi(fixtureEnv(fixture::kEnvTrackRoleId).c_str());
  const std::string pl_name = fixture::kPrefix + " Playlist Test";

  std::cout << "== 主服务播放列表接口 ==\n";

  Response r =
      test_http::httpRequest(host, port, "POST", "/api/playlists/refresh");
  check(r.status == 200, "POST /api/playlists/refresh -> 200");

  r = test_http::httpRequest(host, port, "POST", "/api/playlists",
                             "{\"name\":\"" + pl_name + "\"}");
  check(r.status == 200, "POST /api/playlists -> 200");
  std::string pl_id;
  {
    auto j = json_util::parseOrNull(r.body);
    if (!j.isNull() && j.isMember("id")) {
      pl_id = j["id"].asString();
    }
  }
  check(!pl_id.empty(), "新建歌单返回 id");
  if (pl_id.empty()) {
    std::cout << "无法继续（未创建歌单）\n";
    return 1;
  }

  r = test_http::httpRequest(host, port, "GET", "/api/playlists/" + pl_id);
  check(r.status == 200, "GET /api/playlists/<id> -> 200");
  {
    auto j = json_util::parseOrNull(r.body);
    check(!j.isNull() && j.isMember("song_count") &&
              j["song_count"].asInt64() == 0,
          "新歌单 song_count=0");
  }

  r = test_http::httpRequest(host, port, "GET", "/api/playlists");
  check(r.status == 200, "GET /api/playlists -> 200");

  r = test_http::httpRequest(host, port, "POST", "/api/playlists/" + pl_id +
                                                   "/songs",
                             "{\"trackId\":" + std::to_string(track_id) + "}");
  check(r.status == 200, "POST /api/playlists/<id>/songs (trackId) -> 200");
  {
    auto j = json_util::parseOrNull(r.body);
    check(!j.isNull() && j.isMember("song_count") &&
              j["song_count"].asInt64() == 1,
          "添加后 song_count=1");
    bool matched = false;
    if (!j.isNull() && j.isMember("songs") && j["songs"].size() > 0) {
      matched = j["songs"][0]["matched"].asBool();
    }
    check(matched, "添加的曲目 matched=true");
  }

  r = test_http::httpRequest(host, port, "DELETE",
                             "/api/playlists/" + pl_id + "/songs/0");
  check(r.status == 200, "DELETE /api/playlists/<id>/songs/0 -> 200");
  r = test_http::httpRequest(host, port, "GET", "/api/playlists/" + pl_id);
  {
    auto j = json_util::parseOrNull(r.body);
    check(!j.isNull() && j["song_count"].asInt64() == 0,
          "删除曲目后 song_count=0");
  }

  r = test_http::httpRequest(host, port, "DELETE", "/api/playlists/" + pl_id);
  check(r.status == 200, "DELETE /api/playlists/<id> -> 200");
  r = test_http::httpRequest(host, port, "GET", "/api/playlists/" + pl_id);
  check(r.status == 404, "删除后 GET /api/playlists/<id> -> 404");

  std::cout << "\n== Jellyfin 播放列表写入（保持原有 501）==\n";
  // Jellyfin 歌单写入从未注册能力，迁移前后都返回 501；此处锁定该行为。
  r = test_http::httpRequest(host, jf_port, "POST",
                             "/Playlists?name=" + fixture::kPrefix +
                                 "%20Unsupported");
  check(r.status == 501, "POST /Playlists -> 501");
  r = test_http::httpRequest(
      host, jf_port, "POST", "/Playlists",
      "{\"Name\":\"" + fixture::kPrefix + "Body Unsupported\",\"Ids\":[]}");
  check(r.status == 501, "POST /Playlists (JSON body) -> 501");

  std::cout << "\n== Jellyfin 读取主服务歌单 ==\n";
  // 歌单是 JSON 文件存储，唯一写入口是主服务 API；先建一个再让 Jellyfin 读。
  const std::string track_ref = fixtureEnv(fixture::kEnvTrackRoleRef);
  std::string jf_read_id;
  r = test_http::httpRequest(
      host, port, "POST", "/api/playlists",
      "{\"name\":\"" + fixture::kPrefix + " Jellyfin Read\"}");
  if (auto j = json_util::parseOrNull(r.body); has(j, "id")) {
    jf_read_id = j["id"].asString();
  }
  check(!jf_read_id.empty(), "创建 Jellyfin 读取用歌单");
  const std::string pl_ref = "local-main:playlist:" + jf_read_id;
  if (!jf_read_id.empty()) {
    r = test_http::httpRequest(
        host, port, "POST", "/api/playlists/" + jf_read_id + "/songs",
        "{\"trackId\":" + std::to_string(track_id) + "}");
    check(r.status == 200, "向该歌单添加夹具曲目");
  }

  r = test_http::httpRequest(host, jf_port, "GET", "/Playlists/" + pl_ref);
  check(r.status == 200, "GET /Playlists/<ref> -> 200");
  {
    auto j = json_util::parseOrNull(r.body);
    check(has(j, "Id") && j["Id"].asString() == pl_ref,
          "歌单 Id 与引用一致");
    check(has(j, "ChildCount") && j["ChildCount"].asInt64() == 1,
          "歌单 ChildCount=1");
    bool found = false;
    if (has(j, "ItemIds")) {
      for (const auto &x : j["ItemIds"]) {
        if (x.asString() == track_ref) {
          found = true;
        }
      }
    }
    check(found, "ItemIds 含夹具曲目");
    check(has(j, "RunTimeTicks") && j["RunTimeTicks"].asInt64() > 0,
          "歌单 RunTimeTicks>0");
  }

  r = test_http::httpRequest(host, jf_port, "GET",
                             "/Playlists/" + pl_ref + "/Items");
  check(r.status == 200, "GET /Playlists/<ref>/Items -> 200");
  {
    auto j = json_util::parseOrNull(r.body);
    check(has(j, "Items") && j["Items"].isArray() && j["Items"].size() == 1,
          "歌单曲目列表含 1 条");
  }

  r = test_http::httpRequest(host, jf_port, "GET", "/Items/" + pl_ref);
  check(r.status == 200, "GET /Items/<playlistRef> -> 200");
  r = test_http::httpRequest(
      host, jf_port, "GET",
      "/Users/0123456789abcdef0123456789abcdef/Items/" + pl_ref);
  check(r.status == 200, "GET /Users/<uid>/Items/<playlistRef> -> 200");

  r = test_http::httpRequest(host, jf_port, "POST",
                             "/Playlists/" + pl_ref + "/Items?ids=" + track_ref);
  check(r.status == 501, "POST /Playlists/<ref>/Items -> 501");
  r = test_http::httpRequest(host, jf_port, "DELETE",
                             "/Playlists/" + pl_ref + "/Items?entryIds=" +
                                 track_ref);
  check(r.status == 501, "DELETE /Playlists/<ref>/Items -> 501");
  r = test_http::httpRequest(host, jf_port, "DELETE", "/Playlists/" + pl_ref);
  check(r.status == 501, "DELETE /Playlists/<ref> -> 501");
  if (!jf_read_id.empty()) {
    r = test_http::httpRequest(host, port, "DELETE",
                               "/api/playlists/" + jf_read_id);
    check(r.status == 200, "清理 Jellyfin 读取用歌单");
  }

  std::cout << "\n== 专辑时长 ==\n";
  {
    const std::string album_ref = fixtureEnv(fixture::kEnvAlbumRef);
    r = test_http::httpRequest(host, jf_port, "GET", "/Items/" + album_ref);
    auto j = json_util::parseOrNull(r.body);
    check(has(j, "RunTimeTicks") && j["RunTimeTicks"].asInt64() > 0,
          "专辑 RunTimeTicks>0（总时长）");
  }

  std::cout << "\n结果: PASS=" << g_pass << " FAIL=" << g_fail << "\n";
  return g_fail == 0 ? 0 : 1;
}