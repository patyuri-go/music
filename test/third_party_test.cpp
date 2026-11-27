// 第三方门面（Subsonic / Jellyfin）字节出口验收测试。
//
// 用法：build/bin/test/third_party_test [host] [main_port] [jellyfin_port]
//                                      [subsonic_port] [storage_port]
//       默认 127.0.0.1:5667 / :9000 / :4533 / :15666
//
// 关注两件事：
//   1. 字节出口与原生 api 完全一致 —— 远程流是 push + chunked，绝不带
//      Content-Length；Range 仍然生效（206 + Content-Range）。
//   2. 错误体按各自协议信封输出，响应头不分协议（Retry-After 照发）。
//
// 退出码 0 表示全部通过。

#include "fixture_ids.h"
#include "test_http.h"

#include <json/json.h>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

struct Config {
  std::string host{"127.0.0.1"};
  int port{5667};
  int jf_port{9000};
  int subsonic_port{4533};
  int storage_port{15666};
};

Response get(const Config &cfg, int port, const std::string &path,
             const std::vector<std::string> &headers = {}) {
  return test_http::httpRequest(cfg.host, port, "GET", path, {}, headers);
}

Response head(const Config &cfg, int port, const std::string &path) {
  return test_http::httpRequest(cfg.host, port, "HEAD", path);
}

Json::Value parse(const std::string &body) {
  Json::Value value;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::istringstream input(body);
  if (!Json::parseFromStream(builder, input, &value, &errors)) {
    return Json::Value();
  }
  return value;
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

std::string urlEncodeRef(const std::string &ref) {
  // 只编码 query 里会咬人的字符；冒号在 query 中是合法的。
  std::string out;
  for (char c : ref) {
    if (c == '&' || c == '#' || c == '+' || c == ' ') {
      out += '%';
      const char hex[] = "0123456789ABCDEF";
      out += hex[(static_cast<unsigned char>(c) >> 4) & 0xF];
      out += hex[static_cast<unsigned char>(c) & 0xF];
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// 三个门面必须给出完全一致的字节契约：远程流 chunked、无 Content-Length、
// 仍然声明 Accept-Ranges。
void checkChunkedStream(const Response &res, const std::string &label,
                        std::size_t expect_bytes) {
  check(res.status == 200,
        label + " -> 200（实际 " + std::to_string(res.status) + "）");
  check(res.header("content-length").empty(),
        label + " 不得带 Content-Length（实际 " + res.header("content-length") +
            "）");
  check(res.header("transfer-encoding") == "chunked",
        label + " 是 chunked（实际 " + res.header("transfer-encoding") + "）");
  check(res.header("accept-ranges") == "bytes",
        label + " 声明 Accept-Ranges: bytes（实际 " +
            res.header("accept-ranges") + "）");
  if (expect_bytes > 0) {
    check(res.body.size() == expect_bytes,
        label + " 字节数 = " + std::to_string(expect_bytes) + "（实际 " +
            std::to_string(res.body.size()) + "）");
  }
}

void checkRangeStream(const Response &res, const std::string &label,
                      std::size_t expect_bytes, const std::string &total) {
  check(res.status == 206,
        label + " -> 206（实际 " + std::to_string(res.status) + "）");
  check(res.header("content-length").empty(),
        label + " 不得带 Content-Length（实际 " + res.header("content-length") +
            "）");
  check(res.header("transfer-encoding") == "chunked",
        label + " 是 chunked（实际 " + res.header("transfer-encoding") + "）");
  check(res.header("content-range") ==
            "bytes 0-" + std::to_string(expect_bytes - 1) + "/" + total,
        label + " Content-Range（实际 " + res.header("content-range") + "）");
  check(res.body.size() == expect_bytes,
        label + " 字节数 = " + std::to_string(expect_bytes) + "（实际 " +
            std::to_string(res.body.size()) + "）");
}

void runSubsonic(const Config &cfg) {
  const std::string track_ref = requireEnv(fixture::kEnvTrackRoleRef);
  const std::string album_ref = requireEnv(fixture::kEnvAlbumRef);
  const std::string unlinked_ref = requireEnv(fixture::kEnvTrackUnlinkedNullRef);
  const std::string total = std::to_string(fixture::kSizeBytes);

  std::cout << "\n== Subsonic：字节出口与原生一致 ==\n";
  const std::string stream_path =
      "/rest/stream?id=" + urlEncodeRef(track_ref);
  checkChunkedStream(get(cfg, cfg.subsonic_port, stream_path),
                     "getStream(GET)", fixture::kSizeBytes);

  // HEAD 走同一个池，只回头。已知长度时仍发 Content-Length。
  const Response head_res = head(cfg, cfg.subsonic_port, stream_path);
  check(head_res.status == 200,
        "getStream(HEAD) -> 200（实际 " + std::to_string(head_res.status) + "）");
  check(head_res.body.empty(), "getStream(HEAD) 无 body");
  check(head_res.header("content-length") == total,
        "getStream(HEAD) Content-Length = " + total + "（实际 " +
            head_res.header("content-length") + "）");

  checkRangeStream(
      get(cfg, cfg.subsonic_port, stream_path, {"Range: bytes=0-1023"}),
      "getStream(Range)", 1024, total);

  // download 与 stream 是同一字节来源，Range 语义也一致。
  const std::string download_path =
      "/rest/download?id=" + urlEncodeRef(track_ref);
  checkRangeStream(
      get(cfg, cfg.subsonic_port, download_path, {"Range: bytes=0-511"}),
      "download(Range)", 512, total);

  std::cout << "\n== Subsonic：错误体用 subsonic-response 信封 ==\n";
  // 没有 entity、也没有链接的曲目：解析成功但取流失败，走 worker 内错误分支。
  const Response not_found =
      get(cfg, cfg.subsonic_port,
          "/rest/stream?id=" + urlEncodeRef(unlinked_ref));
  check(not_found.status == 200,
        "取流失败仍回 HTTP 200（Subsonic 规范，实际 " +
            std::to_string(not_found.status) + "）");
  const Json::Value envelope = parse(not_found.body);
  check(envelope.isMember("subsonic-response"),
        "错误体是 subsonic-response 信封（实际 " + not_found.body + "）");
  const Json::Value &sr = envelope["subsonic-response"];
  check(sr.isObject() && sr["status"].asString() == "failed",
        "信封 status = failed");
  check(sr.isObject() && sr.isMember("error") && sr["error"].isObject(),
        "信封带 error 对象");
  check(sr.isObject() && sr["error"]["code"].asInt() == 70,
        "not_found -> Subsonic code 70（实际 " +
            std::to_string(sr["error"]["code"].asInt()) + "）");

  // 取流路径上不存在的引用同样是信封，而不是裸 JSON。
  const Response bad_ref =
      get(cfg, cfg.subsonic_port, "/rest/stream?id=local-main:track:999999");
  check(bad_ref.status == 200, "未知引用回 HTTP 200（实际 " +
                                   std::to_string(bad_ref.status) + "）");
  const Json::Value bad_env = parse(bad_ref.body);
  check(bad_env.isMember("subsonic-response") &&
            bad_env["subsonic-response"]["status"].asString() == "failed",
        "未知引用也走 subsonic-response 信封");

  std::cout << "\n== Subsonic：封面 ==\n";
  const std::string cover_path =
      "/rest/getCoverArt?id=" + urlEncodeRef(album_ref);
  const Response cover = get(cfg, cfg.subsonic_port, cover_path);
  check(cover.status == 200,
        "getCoverArt -> 200（实际 " + std::to_string(cover.status) + "）");
  check(cover.body.size() >= 2 &&
            static_cast<unsigned char>(cover.body[0]) == 0xFF &&
            static_cast<unsigned char>(cover.body[1]) == 0xD8,
        "封面字节含 JPEG magic");
  const std::string etag = cover.header("etag");
  check(!etag.empty(), "封面带 ETag");
  const Response not_modified =
      get(cfg, cfg.subsonic_port, cover_path, {"If-None-Match: " + etag});
  check(not_modified.status == 304,
        "If-None-Match 命中 -> 304（实际 " +
            std::to_string(not_modified.status) + "）");

  std::cout << "\n== Subsonic：上游错误也走信封 ==\n";
  const std::string plain_entity = requireEnv(fixture::kEnvTrackPlainEntity);
  const std::string plain_ref = requireEnv(fixture::kEnvTrackPlainRef);
  control(cfg, plain_entity, "{\"status\":500}");
  const Response upstream =
      get(cfg, cfg.subsonic_port, "/rest/stream?id=" + urlEncodeRef(plain_ref));
  check(upstream.status == 200,
        "上游 500 仍回 HTTP 200（实际 " + std::to_string(upstream.status) + "）");
  const Json::Value upstream_env = parse(upstream.body);
  check(upstream_env.isMember("subsonic-response") &&
            upstream_env["subsonic-response"]["status"].asString() == "failed",
        "上游 500 走 subsonic-response 信封（实际 " + upstream.body + "）");
  resetStorage(cfg);
}

void runJellyfin(const Config &cfg) {
  const std::string track_ref = requireEnv(fixture::kEnvTrackRoleRef);
  const std::string album_ref = requireEnv(fixture::kEnvAlbumRef);
  const std::string unlinked_ref = requireEnv(fixture::kEnvTrackUnlinkedNullRef);
  const std::string total = std::to_string(fixture::kSizeBytes);

  std::cout << "\n== Jellyfin：字节出口与原生一致 ==\n";
  const std::string stream_path = "/Audio/" + track_ref + "/stream";
  checkChunkedStream(get(cfg, cfg.jf_port, stream_path), "GET /Audio/.../stream",
                     fixture::kSizeBytes);

  const Response head_res = head(cfg, cfg.jf_port, stream_path);
  check(head_res.status == 200,
        "HEAD /Audio/.../stream -> 200（实际 " +
            std::to_string(head_res.status) + "）");
  check(head_res.body.empty(), "HEAD /Audio 无 body");
  check(head_res.header("content-length") == total,
        "HEAD /Audio Content-Length = " + total + "（实际 " +
            head_res.header("content-length") + "）");

  checkRangeStream(
      get(cfg, cfg.jf_port, stream_path, {"Range: bytes=0-1023"}),
      "GET /Audio(Range)", 1024, total);

  // /Items/<id>/Download 与 /File 同样走 push。
  const std::string download_path = "/Items/" + track_ref + "/Download";
  checkRangeStream(
      get(cfg, cfg.jf_port, download_path, {"Range: bytes=0-255"}),
      "GET /Items/.../Download(Range)", 256, total);

  std::cout << "\n== Jellyfin：错误体用 ProblemDetails ==\n";
  const Response not_found =
      get(cfg, cfg.jf_port, "/Audio/" + unlinked_ref + "/stream");
  check(not_found.status == 404,
        "取流失败 -> 404（实际 " + std::to_string(not_found.status) + "）");
  check(not_found.header("content-type").find("json") != std::string::npos,
        "错误体是 JSON（实际 " + not_found.header("content-type") + "）");
  const Json::Value problem = parse(not_found.body);
  check(problem.isObject() && problem.isMember("status"),
        "错误体符合 ProblemDetails（带 status 字段，实际 " + not_found.body +
            "）");
  check(problem.isObject() && problem["status"].asInt() == 404,
        "ProblemDetails.status = 404（实际 " +
            std::to_string(problem["status"].asInt()) + "）");
  check(problem.isObject() && problem.isMember("title"),
        "ProblemDetails 带 title");
  check(problem.isObject() && !problem.isMember("error"),
        "错误体不是原生 /api 的 {\"error\":...} 形态");

  // 不存在的引用在 handler 内就被挡下，不进 worker。
  const Response bad_ref =
      get(cfg, cfg.jf_port, "/Audio/not-a-ref/stream");
  check(bad_ref.status == 404,
        "未知引用 -> 404（实际 " + std::to_string(bad_ref.status) + "）");

  std::cout << "\n== Jellyfin：封面 ==\n";
  const std::string cover_path = "/Items/" + album_ref + "/Images/Primary";
  const Response cover = get(cfg, cfg.jf_port, cover_path);
  check(cover.status == 200,
        "GET /Items/.../Images/Primary -> 200（实际 " +
            std::to_string(cover.status) + "）");
  check(cover.body.size() >= 2 &&
            static_cast<unsigned char>(cover.body[0]) == 0xFF &&
            static_cast<unsigned char>(cover.body[1]) == 0xD8,
        "封面字节含 JPEG magic");
  check(!cover.header("etag").empty(), "封面带 ETag");

  std::cout << "\n== Jellyfin：上游错误 -> ProblemDetails ==\n";
  const std::string plain_entity = requireEnv(fixture::kEnvTrackPlainEntity);
  const std::string plain_ref = requireEnv(fixture::kEnvTrackPlainRef);
  control(cfg, plain_entity, "{\"status\":500}");
  const Response upstream =
      get(cfg, cfg.jf_port, "/Audio/" + plain_ref + "/stream");
  check(upstream.status == 503,
        "上游 500 -> 503（实际 " + std::to_string(upstream.status) + "）");
  const Json::Value upstream_problem = parse(upstream.body);
  check(upstream_problem.isObject() && upstream_problem["status"].asInt() == 503,
        "上游错误体是 ProblemDetails(status=503)（实际 " + upstream.body + "）");
  resetStorage(cfg);
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
    cfg.jf_port = std::atoi(argv[3]);
  }
  if (argc > 4) {
    cfg.subsonic_port = std::atoi(argv[4]);
  }
  if (argc > 5) {
    cfg.storage_port = std::atoi(argv[5]);
  }
  (void)cfg.port;

  runSubsonic(cfg);
  runJellyfin(cfg);

  std::cout << "\n结果: PASS=" << g_pass << " FAIL=" << g_fail << "\n";
  return g_fail == 0 ? 0 : 1;
}
