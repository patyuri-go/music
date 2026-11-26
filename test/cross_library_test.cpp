// 跨库取流验收测试：本地曲目在 entity_link 上挂到网易云后，
// 路由要回退到远端库并真的把字节代理出来。
//
// 用法：build/bin/test/cross_library_test [host] [port]
// 依赖 run_integration.sh 导出的夹具环境变量、假 PostgreSQL 与假网易云 API。
// 退出码 0 表示全部通过。

#include "fixture_ids.h"
#include "test_http.h"

#include <json/json.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using test_http::Response;

std::string env(const char *key, const std::string &fallback = {}) {
  const char *value = std::getenv(key);
  return value == nullptr ? fallback : std::string{value};
}

std::string requireEnv(const char *key) {
  const std::string value = env(key);
  if (value.empty()) {
    std::cerr << "缺少环境变量 " << key << "（先运行 test_fixture seed）\n";
    std::exit(2);
  }
  return value;
}

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

struct Config {
  std::string host{"127.0.0.1"};
  int port{5667};
  int storage_port{15666};
};

Response get(const Config &cfg, const std::string &path) {
  return test_http::httpRequest(cfg.host, cfg.port, "GET", path);
}

Response post(const Config &cfg, const std::string &path,
              const std::string &body = {}) {
  return test_http::httpRequest(cfg.host, cfg.port, "POST", path, body);
}

// 假存储的控制面：按实体 id 改状态，用来制造「存储读不到」。
Response controlStorage(const Config &cfg, const std::string &entity_id,
                        const std::string &body) {
  return test_http::httpRequest(cfg.host, cfg.storage_port, "POST",
                                "/__control/entity/" + entity_id, body);
}

bool parseJson(const std::string &text, Json::Value &out) {
  Json::CharReaderBuilder builder;
  std::string errors;
  std::istringstream stream(text);
  return Json::parseFromStream(builder, stream, &out, &errors);
}

// 链路里出现过的库 id（按尝试顺序）。
std::vector<std::string> chainLibraries(const Json::Value &origin) {
  std::vector<std::string> out;
  for (const Json::Value &hop : origin["chain"]) {
    out.push_back(hop["library_id"].asString());
  }
  return out;
}

std::size_t countLibrary(const std::vector<std::string> &items,
                         const std::string &id) {
  return static_cast<std::size_t>(
      std::count(items.begin(), items.end(), id));
}

std::string join(const std::vector<std::string> &items) {
  std::string out;
  for (const std::string &item : items) {
    if (!out.empty()) {
      out += ",";
    }
    out += item;
  }
  return out;
}

} // namespace

int main(int argc, char **argv) {
  Config cfg;
  if (argc > 1) {
    cfg.host = argv[1];
  }
  if (argc > 2) {
    cfg.port = std::atoi(argv[2]);
  }
  if (argc > 3) {
    cfg.storage_port = std::atoi(argv[3]);
  }

  // 12 号曲目：本地 entity_id 为 NULL，但挂在跨库链接上。
  const std::string linked_ref = requireEnv("FIXTURE_TRACK_NULL_REF");
  // 14 号曲目：本地 entity_id 为 NULL，没有链接（仍应 404）。
  const std::string unlinked_ref = requireEnv("FIXTURE_TRACK_UNLINKED_NULL_REF");
  const std::string remote_ref =
      fixture::kNeteaseLibraryId + ":track:" + fixture::kNeteaseSongId;
  const std::string blocked_ref =
      fixture::kNeteaseLibraryId + ":track:" + fixture::kNeteaseBlockedSongId;

  std::cout << "== 映射表可读 ==\n";
  const Response link = get(cfg, "/api/v1/entity-links/" + linked_ref);
  check(link.status == 200, "GET entity-links -> 200（实际 " +
                                std::to_string(link.status) + "）");
  Json::Value link_body;
  const bool link_parsed = parseJson(link.body, link_body);
  check(link_parsed, "映射表响应是 JSON");
  if (link_parsed) {
    std::vector<std::string> libraries;
    for (const Json::Value &ref : link_body["refs"]) {
      libraries.push_back(ref["library_id"].asString());
    }
    check(countLibrary(libraries, fixture::kNeteaseLibraryId) == 1,
          "链接里有网易云成员（" + join(libraries) + "）");
    check(countLibrary(libraries, "local-main") == 1,
          "链接里有本地成员（" + join(libraries) + "）");
  }

  const Response order =
      get(cfg, "/api/v1/entity-links/" + linked_ref + "/order?aspect=stream");
  check(order.status == 200, "GET entity-links order -> 200");
  Json::Value order_body;
  const bool order_parsed = parseJson(order.body, order_body);
  check(order_parsed, "顺序响应是 JSON");
  if (order_parsed) {
    const Json::Value &entries = order_body["order"]["stream"];
    check(entries.size() == 2, "stream 顺序有 2 项");
    if (entries.size() == 2) {
      // 临时配置的 library_priority 把网易云排在前面，DB override 却把本地排
      // 前面；这里拿到本地就证明 override（effective_order 第一级）生效了。
      check(entries[0]["library_id"].asString() == "local-main",
            "override 把本地排第一（实际 " +
                entries[0]["library_id"].asString() + "）");
      check(entries[1]["library_id"].asString() == fixture::kNeteaseLibraryId,
            "override 把网易云排第二（实际 " +
                entries[1]["library_id"].asString() + "）");
    }
  }

  std::cout << "\n== 会话路由与回退链 ==\n";
  const Response opened = post(cfg, "/api/v1/playback/" + linked_ref);
  check(opened.status == 201, "POST playback -> 201（实际 " +
                                  std::to_string(opened.status) + "）");
  Json::Value session;
  const bool session_parsed = parseJson(opened.body, session);
  check(session_parsed, "会话响应是 JSON");

  std::string stream_url;
  if (session_parsed) {
    check(session["track"]["origin"]["library_id"].asString() == "local-main",
          "元数据来自本地（实际 " +
              session["track"]["origin"]["library_id"].asString() + "）");
    check(session["stream"]["origin"]["library_id"].asString() ==
              fixture::kNeteaseLibraryId,
          "流来自网易云（实际 " +
              session["stream"]["origin"]["library_id"].asString() + "）");
    check(session["stream"]["origin"]["substituted"].asBool(),
          "流标记为已替换来源");

    const Json::Value &chain = session["stream"]["origin"]["chain"];
    check(chain.size() == 2, "回退链 2 跳（实际 " +
                                 std::to_string(chain.size()) + "）");
    if (chain.size() == 2) {
      check(chain[0]["library_id"].asString() == "local-main",
            "第 1 跳是本地");
      check(!chain[0]["ok"].asBool(), "第 1 跳失败");
      check(chain[0]["error"].asString() == "not_found",
            "第 1 跳原因是 not_found（实际 " +
                chain[0]["error"].asString() + "）");
      check(chain[1]["library_id"].asString() == fixture::kNeteaseLibraryId,
            "第 2 跳是网易云");
      check(chain[1]["ok"].asBool(), "第 2 跳成功");
    }
    stream_url = session["stream_url"].asString();
  }

  std::cout << "\n== 远端字节代理 ==\n";
  if (!stream_url.empty()) {
    const Response media = get(cfg, stream_url);
    check(media.status == 200, "GET 会话流 -> 200（实际 " +
                                   std::to_string(media.status) + "）");
    check(media.header("x-musicd-origin") == fixture::kNeteaseLibraryId,
          "X-Musicd-Origin 指向网易云（实际 " +
              media.header("x-musicd-origin") + "）");
    check(media.header("content-length") ==
              std::to_string(fixture::kNeteaseAudioSize),
          "Content-Length = " + std::to_string(fixture::kNeteaseAudioSize) +
              "（实际 " + media.header("content-length") + "）");
    check(media.body == fixture::kNeteaseAudioBytes(), "字节与假 API 一致");
  } else {
    check(false, "会话没有 stream_url，后续字节断言跳过");
  }

  std::cout << "\n== 直连远程引用 ==\n";
  const Response remote = get(cfg, "/api/v1/tracks/" + remote_ref + "/stream");
  check(remote.status == 200, "GET 远程曲目流 -> 200（实际 " +
                                  std::to_string(remote.status) + "）");
  check(remote.header("x-musicd-origin") == fixture::kNeteaseLibraryId,
        "直连流的 X-Musicd-Origin 是网易云");
  check(remote.body == fixture::kNeteaseAudioBytes(),
        "直连流字节与假 API 一致");

  std::cout << "\n== 失败分支 ==\n";
  const Response blocked = post(cfg, "/api/v1/playback/" + blocked_ref);
  check(blocked.status == 403, "版权不可用 -> 403（实际 " +
                                   std::to_string(blocked.status) + "）");

  std::cout << "\n== 主路由也要能跨库 ==\n";
  const Response direct = get(cfg, "/api/v1/tracks/" + linked_ref + "/stream");
  check(direct.status == 200, "GET 本地无实体但有链接 -> 200（实际 " +
                                  std::to_string(direct.status) + "）");
  check(direct.header("x-musicd-origin") == fixture::kNeteaseLibraryId,
        "主路由的 X-Musicd-Origin 是网易云（实际 " +
            direct.header("x-musicd-origin") + "）");
  check(direct.body == fixture::kNeteaseAudioBytes(), "主路由字节与假 API 一致");

  std::cout << "\n== 本地有实体但存储读不到 ==\n";
  // 10 号曲目的 entity_id 非空，并挂在 2 号链接上（远端 186018）。
  // 本地 resolve_playback 会先探一次存储（Range 0-），所以这里把存储打成
  // 500 后它报 ProviderUnavailable，路由按 should_fall_back 换网易云。
  const std::string entity_ref = requireEnv("FIXTURE_TRACK_ROLE_REF");
  const std::string entity_id = requireEnv("FIXTURE_TRACK_ROLE_ENTITY");
  const Response broke = controlStorage(cfg, entity_id, R"({"status":500})");
  check(broke.status == 200, "把本地曲目的存储状态改成 500");

  const Response entity_opened = post(cfg, "/api/v1/playback/" + entity_ref);
  check(entity_opened.status == 201, "存储 500 时 POST playback -> 201（实际 " +
                                         std::to_string(entity_opened.status) +
                                         "）");
  Json::Value entity_session;
  const bool entity_parsed = parseJson(entity_opened.body, entity_session);
  check(entity_parsed, "存储 500 的会话响应是 JSON");
  if (entity_parsed) {
    check(entity_session["stream"]["origin"]["library_id"].asString() ==
              fixture::kNeteaseLibraryId,
          "存储 500 时流应回退到网易云（实际 " +
              entity_session["stream"]["origin"]["library_id"].asString() + "）");
    const std::string entity_stream = entity_session["stream_url"].asString();
    check(!entity_stream.empty(), "存储 500 的会话有 stream_url");
    if (!entity_stream.empty()) {
      const Response media = get(cfg, entity_stream);
      check(media.status == 200, "存储 500 后取流 -> 200（实际 " +
                                     std::to_string(media.status) + "）");
      check(media.header("x-musicd-origin") == fixture::kNeteaseLibraryId,
            "存储 500 后取流的来源是网易云（实际 " +
                media.header("x-musicd-origin") + "）");
      check(media.body == fixture::kNeteaseEntityAudioBytes(),
            "存储 500 后拿到的是备份源的字节");
    }
  }

  // 无状态入口（客户端最常用的 /tracks/<ref>/stream）也必须在同一次请求里换源。
  const Response main_route =
      get(cfg, "/api/v1/tracks/" + entity_ref + "/stream");
  check(main_route.status == 200, "存储 500 时主路由 -> 200（实际 " +
                                      std::to_string(main_route.status) + "）");
  check(main_route.header("x-musicd-origin") == fixture::kNeteaseLibraryId,
        "存储 500 时主路由的来源是网易云（实际 " +
            main_route.header("x-musicd-origin") + "）");
  check(main_route.body == fixture::kNeteaseEntityAudioBytes(),
        "存储 500 时主路由拿到的是备份源的字节");

  // 复位，别影响后面的段。
  controlStorage(cfg, entity_id, R"({"status":200,"delay_ms":0})");

  std::cout << "\n== 无链接仍硬失败 ==\n";
  const Response unlinked =
      get(cfg, "/api/v1/tracks/" + unlinked_ref + "/stream");
  check(unlinked.status == 404, "无链接且无实体 -> 404（实际 " +
                                    std::to_string(unlinked.status) + "）");

  std::cout << "\n结果: PASS=" << g_pass << " FAIL=" << g_fail << "\n";
  return g_fail == 0 ? 0 : 1;
}