// 歌词功能测试（C++，走真实 HTTP + TagLib 写入内嵌歌词）。
// 用法：先启动 musicd（主 5666 / Jellyfin 9000），再运行本程序。
//
//   ./lyrics_test                          # 默认源文件 test_music/Artist A/Album One/01 - First Song.wav
//   ./lyrics_test <source_audio> [host] [port] [jellyfin_port]
//
// 退出码：0 表示全部通过，非 0 表示有失败。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/toolkit/tpropertymap.h>

#include "crow/json.h"

namespace fs = std::filesystem;

namespace {

struct HttpResponse {
  int status = 0;
  std::string body;
};

HttpResponse httpRequest(const std::string &host, int port,
                         const std::string &method, const std::string &path,
                         const std::string &body = std::string()) {
  HttpResponse resp;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return resp;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return resp;
  }
  std::ostringstream req;
  req << method << " " << path << " HTTP/1.1\r\n";
  req << "Host: " << host << ":" << port << "\r\n";
  req << "Connection: close\r\n";
  if (!body.empty()) {
    req << "Content-Length: " << body.size() << "\r\n";
    req << "Content-Type: application/json\r\n";
  }
  req << "\r\n" << body;
  const std::string raw_req = req.str();
  ::send(fd, raw_req.data(), raw_req.size(), 0);

  std::string raw;
  char buf[8192];
  ssize_t n = 0;
  while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
    raw.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);

  const std::size_t head_end = raw.find("\r\n\r\n");
  if (head_end == std::string::npos) {
    return resp;
  }
  const std::string head = raw.substr(0, head_end);
  resp.body = raw.substr(head_end + 4);
  const std::size_t sp = head.find(' ');
  if (sp != std::string::npos) {
    resp.status = std::atoi(head.c_str() + sp + 1);
  }
  return resp;
}

std::string trackGuid(int track_id) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%08x%024x", 1, track_id);
  return std::string(buf);
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

bool embedLyrics(const std::string &path, const std::string &lyrics) {
  TagLib::FileRef f(path.c_str());
  if (f.isNull() || !f.file()) {
    return false;
  }
  auto props = f.file()->properties();
  TagLib::StringList sl;
  sl.append(TagLib::String(lyrics, TagLib::String::UTF8));
  props.replace("LYRICS", sl);
  f.file()->setProperties(props);
  return f.save();
}

// 在 browse 结果里按文件名找 trackId。
int findTrackIdByName(const std::string &host, int port, int root_id,
                      const std::string &name) {
  HttpResponse r = httpRequest(host, port, "GET",
                               "/api/fs/browse/" + std::to_string(root_id));
  auto items = crow::json::load(r.body);
  if (!items || items.t() != crow::json::type::List) {
    return 0;
  }
  for (const auto &it : items) {
    if (it.has("name") && it.has("trackId") &&
        std::string(it["name"].s()) == name) {
      return static_cast<int>(it["trackId"].i());
    }
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const std::string source =
      argc > 1 ? argv[1]
               : "/opt/img/test_music/Artist A/Album One/01 - First Song.wav";
  const std::string host = argc > 2 ? argv[2] : "127.0.0.1";
  const int port = argc > 3 ? std::atoi(argv[3]) : 5666;
  const int jf_port = argc > 4 ? std::atoi(argv[4]) : 9000;

  if (!fs::exists(source)) {
    std::cout << "  SKIP  源音频不存在: " << source << "\n";
    std::cout << "\n结果: PASS=" << g_pass << " FAIL=" << g_fail << "\n";
    return 0;
  }

  const std::string dir = "/tmp/opencode/lyricstest2";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  const std::string embedded = dir + "/01 - Embedded.wav";
  const std::string sibling = dir + "/02 - Sibling.wav";
  const std::string nolyrics = dir + "/03 - None.wav";
  fs::copy_file(source, embedded, fs::copy_options::overwrite_existing, ec);
  fs::copy_file(source, sibling, fs::copy_options::overwrite_existing, ec);
  fs::copy_file(source, nolyrics, fs::copy_options::overwrite_existing, ec);

  const std::string synced_lrc =
      "[ti:Embedded]\n[ar:Tester]\n[00:01.50]Embedded line one\n"
      "[00:05.00]Embedded line two\n";
  check(embedLyrics(embedded, synced_lrc), "写入内嵌 LYRICS 成功");
  {
    std::ofstream out(dir + "/02 - Sibling.lrc");
    out << "[ti:Sibling]\n[00:02.00]Sibling line one\n[00:04.00]Sibling line two\n";
  }
  check(fs::exists(dir + "/02 - Sibling.lrc"), "写入同名 .lrc 成功");

  std::cout << "\n== 注册根 + 扫描 ==\n";
  HttpResponse r = httpRequest(host, port, "POST", "/api/roots",
                               "{\"path\":\"" + dir +
                                   "\",\"name\":\"lyricstest2\",\"type\":\"local\"}");
  check(r.status == 200, "POST /api/roots -> 200");
  int root_id = 0;
  {
    auto j = crow::json::load(r.body);
    if (j && j.has("id")) {
      root_id = static_cast<int>(j["id"].i());
    }
  }
  check(root_id > 0, "注册根返回 id");

  if (root_id <= 0) {
    std::cout << "\n结果: PASS=" << g_pass << " FAIL=" << g_fail << "\n";
    return 1;
  }

  r = httpRequest(host, port, "POST", "/api/fs/batch",
                  "[{\"action\":\"scan\",\"rootId\":" + std::to_string(root_id) +
                      ",\"path\":\"" + dir + "\"}]");
  check(r.status == 200, "POST /api/fs/batch scan -> 200");
  std::string job_id;
  {
    auto j = crow::json::load(r.body);
    if (j && j.t() == crow::json::type::List && j.size() > 0 &&
        j[0].has("jobId")) {
      job_id = std::to_string(j[0]["jobId"].i());
    }
  }
  check(!job_id.empty(), "扫描返回 jobId");
  if (!job_id.empty()) {
    bool finished = false;
    for (int k = 0; k < 120 && !finished; ++k) {
      ::usleep(500000);
      r = httpRequest(host, port, "GET", "/api/jobs/" + job_id);
      auto j = crow::json::load(r.body);
      if (j && j.has("state")) {
        const std::string st = j["state"].s();
        if (st == "done" || st == "error") {
          finished = true;
          check(st == "done", "扫描 job 完成 (state=done)");
        }
      }
    }
    if (!finished) {
      check(false, "扫描 job 在超时内完成");
    }
  }

  std::cout << "\n== 内嵌歌词 ==\n";
  const int embedded_id =
      findTrackIdByName(host, port, root_id, "01 - Embedded.wav");
  check(embedded_id > 0, "内嵌歌词曲目已入库");
  if (embedded_id > 0) {
    r = httpRequest(host, jf_port, "GET",
                    "/Audio/" + trackGuid(embedded_id) + "/Lyrics");
    check(r.status == 200, "GET /Audio/<id>/Lyrics (内嵌) -> 200");
    auto j = crow::json::load(r.body);
    check(j && j.has("Lyrics") && j["Lyrics"].size() >= 2,
          "内嵌歌词 Lyrics 行数 >= 2");
    check(j && j.has("Metadata") && j["Metadata"].has("IsSynced") &&
              j["Metadata"]["IsSynced"].b(),
          "内嵌歌词 IsSynced=true");
    if (j && j.has("Lyrics") && j["Lyrics"].size() > 0) {
      check(j["Lyrics"][0].has("Start") && j["Lyrics"][0]["Start"].i() == 15000000,
            "内嵌首行 Start=15000000 ticks");
    }
    r = httpRequest(host, jf_port, "GET", "/Items/" + trackGuid(embedded_id));
    auto it = crow::json::load(r.body);
    check(it && it.has("HasLyrics") && it["HasLyrics"].b(),
          "内嵌曲目 DTO HasLyrics=true");
  }

  std::cout << "\n== 同名 .lrc ==\n";
  const int sibling_id =
      findTrackIdByName(host, port, root_id, "02 - Sibling.wav");
  check(sibling_id > 0, "同名 .lrc 曲目已入库");
  if (sibling_id > 0) {
    r = httpRequest(host, jf_port, "GET",
                    "/Audio/" + trackGuid(sibling_id) + "/Lyrics");
    check(r.status == 200, "GET /Audio/<id>/Lyrics (同名 lrc) -> 200");
    auto j = crow::json::load(r.body);
    check(j && j.has("Lyrics") && j["Lyrics"].size() >= 2,
          "同名 lrc 歌词 Lyrics 行数 >= 2");
  }

  std::cout << "\n== 无歌词 ==\n";
  const int none_id = findTrackIdByName(host, port, root_id, "03 - None.wav");
  check(none_id > 0, "无歌词曲目已入库");
  if (none_id > 0) {
    r = httpRequest(host, jf_port, "GET",
                    "/Audio/" + trackGuid(none_id) + "/Lyrics");
    check(r.status == 404, "GET /Audio/<id>/Lyrics (无歌词) -> 404");
  }

  std::cout << "\n== 清理 ==\n";
  r = httpRequest(host, port, "DELETE", "/api/roots/" + std::to_string(root_id));
  check(r.status == 200, "DELETE /api/roots/<id> -> 200");

  std::cout << "\n结果: PASS=" << g_pass << " FAIL=" << g_fail << "\n";
  return g_fail == 0 ? 0 : 1;
}
