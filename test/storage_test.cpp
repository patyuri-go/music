// 存储代理验收测试：Range、封面类型、空实体、上游故障映射、路由兼容。
//
// 用法：build/bin/test/storage_test [host] [port] [storage_port]
// 依赖 run_integration.sh 导出的夹具环境变量与假存储控制端点。
// 退出码 0 表示全部通过。

#include "test_http.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using test_http::Response;

std::string env(const char *key, const std::string &fallback = {}) {
  const char *value = std::getenv(key);
  return value == nullptr ? fallback : std::string{value};
}

std::string fixture(const char *key) {
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

Response get(const Config &cfg, const std::string &path,
             const std::vector<std::string> &headers = {}) {
  return test_http::httpRequest(cfg.host, cfg.port, "GET", path, {}, headers);
}

Response head(const Config &cfg, const std::string &path) {
  return test_http::httpRequest(cfg.host, cfg.port, "HEAD", path);
}

void control(const Config &cfg, const std::string &entity,
             const std::string &json) {
  test_http::httpRequest(cfg.host, cfg.storage_port, "POST",
                         "/__control/entity/" + entity, json);
}

void resetStorage(const Config &cfg) {
  test_http::httpRequest(cfg.host, cfg.storage_port, "POST",
                         "/__control/reset");
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

  const std::string album_ref = fixture("FIXTURE_ALBUM_REF");
  const std::string album_null_ref = fixture("FIXTURE_ALBUM_NULL_REF");
  const std::string track_ref = fixture("FIXTURE_TRACK_ROLE_REF");
  const std::string track_plain_ref = fixture("FIXTURE_TRACK_PLAIN_REF");
  const std::string track_unlinked_ref =
      fixture("FIXTURE_TRACK_UNLINKED_NULL_REF");
  const std::string track_entity = fixture("FIXTURE_TRACK_ROLE_ENTITY");
  const std::string plain_entity = fixture("FIXTURE_TRACK_PLAIN_ENTITY");
  (void)track_entity; // 该曲目有跨库链接，不适合断言上游错误码

  const std::string stream_path =
      "/api/v1/tracks/" + track_ref + "/stream";
  const std::string legacy_path =
      "/api/track/" + fixture("FIXTURE_TRACK_ROLE_ID") + "/file";

  std::cout << "== 完整文件与 HEAD ==\n";
  const Response full = get(cfg, stream_path);
  check(full.status == 200, "GET stream -> 200");
  check(full.header("accept-ranges") == "bytes", "Accept-Ranges: bytes");
  // 远端字节走 chunked 推送：绝不带 Content-Length（否则头与分帧正文矛盾）。
  check(full.header("content-length").empty(),
        "GET stream 不带 Content-Length（实际 " + full.header("content-length") +
            "）");
  check(full.header("transfer-encoding") == "chunked",
        "GET stream 是 chunked（实际 " + full.header("transfer-encoding") + "）");
  check(full.body.size() == 1048576,
        "完整响应体长度 = 1048576（实际 " +
            std::to_string(full.body.size()) + "）");
  check(full.header("content-type").find("audio") != std::string::npos,
        "Content-Type 为音频（" + full.header("content-type") + "）");

  const Response head_resp = head(cfg, stream_path);
  check(head_resp.status == 200, "HEAD stream -> 200");
  check(head_resp.body.empty(), "HEAD 响应无 body");
  check(head_resp.header("content-length") == "1048576",
        "HEAD Content-Length = 1048576（" +
            head_resp.header("content-length") + "）");

  std::cout << "\n== Range 语义 ==\n";
  struct RangeCase {
    std::string header;
    std::size_t start;
    std::size_t end;
  };
  const std::vector<RangeCase> cases = {
      {"bytes=0-1023", 0, 1023},
      {"bytes=0-", 0, 1048575},
      {"bytes=1000-2023", 1000, 2023},
      {"bytes=-512", 1048064, 1048575},
  };
  for (const auto &item : cases) {
    const Response r = get(cfg, stream_path, {"Range: " + item.header});
    const bool status_ok = r.status == 206 || r.status == 200;
    check(status_ok, item.header + " -> 206/200（实际 " +
                         std::to_string(r.status) + "）");
    const std::size_t expected = item.end - item.start + 1;
    check(r.body.size() == expected,
          item.header + " 长度 " + std::to_string(expected));
    if (r.body.size() == expected && item.end < full.body.size()) {
      check(r.body == full.body.substr(item.start, expected),
            item.header + " 字节与完整响应一致");
    }
    if (r.status == 206) {
      check(r.header("content-range") ==
                "bytes " + std::to_string(item.start) + "-" +
                    std::to_string(item.end) + "/1048576",
            item.header + " Content-Range 正确（" +
                r.header("content-range") + "）");
    }
  }

  const Response unsat =
      get(cfg, stream_path, {"Range: bytes=1048576-"});
  check(unsat.status == 416, "越界 Range -> 416");
  check(unsat.header("content-range") == "bytes */1048576",
        "416 Content-Range: bytes */1048576（" +
            unsat.header("content-range") + "）");

  std::cout << "\n== 封面（按 album.entity_id）==\n";
  const Response cover = get(cfg, "/api/album/" + album_ref + "/cover");
  check(cover.status == 200, "GET album cover -> 200");
  check(cover.header("content-type") == "image/jpeg",
        "封面 Content-Type = image/jpeg（" +
            cover.header("content-type") + "）");
  check(cover.body.size() == 65536, "封面长度 65536");
  check(cover.body.size() >= 4 &&
            static_cast<unsigned char>(cover.body[0]) == 0xFF &&
            static_cast<unsigned char>(cover.body[1]) == 0xD8,
        "封面字节含 JPEG magic");
  check(cover.header("cache-control").find("max-age") != std::string::npos,
        "封面带 Cache-Control");

  // ETag 来源已从存储上游改为路由层的 ArtworkRef.cache_tag（见决策文档
  // 第 16 节），所以这里先取首次响应的 ETag，再用它做条件请求。
  const std::string cover_etag = cover.header("etag");
  check(!cover_etag.empty(), "封面带 ETag");
  const Response cached = get(cfg, "/api/album/" + album_ref + "/cover",
                              {"If-None-Match: " + cover_etag});
  check(cached.status == 304, "If-None-Match 命中 -> 304（实际 " +
                                  std::to_string(cached.status) + "）");
  check(cached.body.empty(), "304 无响应体");
  check(cached.header("etag") == cover_etag,
        "304 保留 ETag（" + cached.header("etag") + "）");
  check(cached.header("cache-control").find("max-age") != std::string::npos,
        "304 保留 Cache-Control");

  std::cout << "\n== 空 entity_id 硬失败 ==\n";
  // 12 号曲目挂到了跨库链接上（见 cross_library_test），这里用 14 号：
  // 同样 entity_id 为 NULL，但没有链接，所以仍应硬失败。
  const Response null_track = get(cfg, "/api/v1/tracks/" + track_unlinked_ref +
                                           "/stream");
  check(null_track.status == 404, "无链接且 entity_id 为 NULL -> 404");
  const Response null_cover = get(cfg, "/api/album/" + album_null_ref +
                                         "/cover");
  check(null_cover.status == 404, "album.entity_id 为 NULL -> 404");

  std::cout << "\n== 存储 302 跳转 ==\n";
  // 存储节点可以把上游直链以 302 交出来（实体位置的 redirect 开关）。
  // 本地库声明跟随跳转，所以这里应当拿到真正的字节，而不是 302 本身。
  control(cfg, plain_entity, "{\"redirect\":true}");
  const Response redirected =
      get(cfg, "/api/v1/tracks/" + track_plain_ref + "/stream");
  check(redirected.status == 200, "302 跟随跳转 -> 200（实际 " +
                                      std::to_string(redirected.status) + "）");
  check(redirected.body.size() == 1048576, "跟随跳转后拿到完整字节");
  const Response redirected_range =
      get(cfg, "/api/v1/tracks/" + track_plain_ref + "/stream",
          {"Range: bytes=0-1023"});
  check(redirected_range.status == 206 && redirected_range.body.size() == 1024,
        "跟随跳转后 Range 仍生效");
  control(cfg, plain_entity, "{\"redirect\":false}");

  std::cout << "\n== 上游状态映射 ==\n";
  // 用没有跨库链接的曲目：有链接时路由会换到备份源，看不到上游错误。
  control(cfg, plain_entity, "{\"status\":500}");
  const Response upstream_500 =
      get(cfg, "/api/v1/tracks/" + track_plain_ref + "/stream");
  check(upstream_500.status == 503, "上游 500 -> 503（实际 " +
                                        std::to_string(upstream_500.status) +
                                        "）");
  resetStorage(cfg);

  control(cfg, plain_entity, "{\"delay_ms\":3000}");
  const Response delayed =
      get(cfg, "/api/v1/tracks/" + track_plain_ref + "/stream");
  check(delayed.status == 504, "上游超时 -> 504（实际 " +
                                   std::to_string(delayed.status) + "）");
  resetStorage(cfg);

  std::cout << "\n== 旧路由兼容 ==\n";
  const Response legacy = get(cfg, legacy_path);
  check(legacy.status == 200, "GET /api/track/<id>/file -> 200");
  check(legacy.body.size() == 1048576, "旧路由返回完整字节");
  const Response legacy_range =
      get(cfg, legacy_path, {"Range: bytes=0-1023"});
  check(legacy_range.status == 206 && legacy_range.body.size() == 1024,
        "旧路由 Range -> 206 + 1024 字节");

  std::cout << "\n结果: PASS=" << g_pass << " FAIL=" << g_fail << "\n";
  return g_fail == 0 ? 0 : 1;
}