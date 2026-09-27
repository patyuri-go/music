// 艺术家解析测试（C++，走真实 HTTP + TagLib 写入 ARTIST 标签）。
// 验证 splitArtists 支持 "Role: A + B | Role: C" 形式与普通 ";"/"/" 分隔。
// 用法：先启动 musicd（主 5666），再运行本程序。
//
//   ./artists_test                          # 默认源文件 test_music/Artist A/Album One/01 - First Song.wav
//   ./artists_test <source_audio> [host] [port]
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
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <taglib/fileref.h>
#include <taglib/tag.h>

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

bool setArtist(const std::string &path, const std::string &artist) {
  TagLib::FileRef f(path.c_str());
  if (f.isNull() || !f.tag()) {
    return false;
  }
  f.tag()->setArtist(TagLib::String(artist, TagLib::String::UTF8));
  return f.save();
}

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

// 取某曲目的艺术家名字集合。
std::vector<std::string> artistNames(const std::string &host, int port,
                                     int track_id) {
  std::vector<std::string> names;
  HttpResponse r =
      httpRequest(host, port, "GET", "/api/track/" + std::to_string(track_id));
  auto t = crow::json::load(r.body);
  if (!t || !t.has("artist_ids")) {
    return names;
  }
  for (const auto &id : t["artist_ids"]) {
    HttpResponse ar = httpRequest(
        host, port, "GET", "/api/artist/" + std::to_string(id.i()));
    auto a = crow::json::load(ar.body);
    if (a && a.has("name")) {
      names.push_back(std::string(a["name"].s()));
    }
  }
  return names;
}

bool hasName(const std::vector<std::string> &names, const std::string &want) {
  for (const auto &n : names) {
    if (n == want) {
      return true;
    }
  }
  return false;
}

} // namespace

int main(int argc, char **argv) {
  const std::string source =
      argc > 1 ? argv[1]
               : "/opt/img/test_music/Artist A/Album One/01 - First Song.wav";
  const std::string host = argc > 2 ? argv[2] : "127.0.0.1";
  const int port = argc > 3 ? std::atoi(argv[3]) : 5666;

  if (!fs::exists(source)) {
    std::cout << "  SKIP  源音频不存在: " << source << "\n";
    std::cout << "\n结果: PASS=" << g_pass << " FAIL=" << g_fail << "\n";
    return 0;
  }

  const std::string dir = "/tmp/opencode/artiststest";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  const std::string credit = dir + "/01 - Credit.wav";
  const std::string plain = dir + "/02 - Plain.wav";
  fs::copy_file(source, credit, fs::copy_options::overwrite_existing, ec);
  fs::copy_file(source, plain, fs::copy_options::overwrite_existing, ec);

  const std::string credit_tag =
      "Compose: typeMARS + GhostFinal | Arrange: typeMARS | "
      "Vocals: \xE7\x91\xB6\xE5\xB1\xB1\xE7\x99\xBE\xE9\x9C\x8A | "
      "Lyrics: TurtleIzzy";
  check(setArtist(credit, credit_tag), "写入角色串 ARTIST 成功");
  check(setArtist(plain, "Artist A; Artist B"), "写入普通多值 ARTIST 成功");

  std::cout << "\n== 注册根 + 扫描 ==\n";
  HttpResponse r = httpRequest(host, port, "POST", "/api/roots",
                               "{\"path\":\"" + dir +
                                   "\",\"name\":\"artiststest\",\"type\":\"local\"}");
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

  std::cout << "\n== 角色串解析 ==\n";
  const int credit_id = findTrackIdByName(host, port, root_id, "01 - Credit.wav");
  check(credit_id > 0, "角色串曲目已入库");
  if (credit_id > 0) {
    const std::vector<std::string> names = artistNames(host, port, credit_id);
    check(names.size() == 4, "角色串解析出 4 位艺术家 (实际 " +
                                 std::to_string(names.size()) + ")");
    check(hasName(names, "typeMARS"), "包含 typeMARS");
    check(hasName(names, "GhostFinal"), "包含 GhostFinal");
    check(hasName(names, "\xE7\x91\xB6\xE5\xB1\xB1\xE7\x99\xBE\xE9\x9C\x8A"),
          "包含 瑶山百霊");
    check(hasName(names, "TurtleIzzy"), "包含 TurtleIzzy");
    check(!hasName(names, "Compose"), "不含角色名 Compose");
    check(!hasName(names, "Arrange"), "不含角色名 Arrange");
  }

  std::cout << "\n== 普通多值 ==\n";
  const int plain_id = findTrackIdByName(host, port, root_id, "02 - Plain.wav");
  check(plain_id > 0, "普通多值曲目已入库");
  if (plain_id > 0) {
    const std::vector<std::string> names = artistNames(host, port, plain_id);
    check(names.size() == 2, "普通多值解析出 2 位艺术家 (实际 " +
                                 std::to_string(names.size()) + ")");
    check(hasName(names, "Artist A"), "包含 Artist A");
    check(hasName(names, "Artist B"), "包含 Artist B");
  }

  std::cout << "\n== 清理 ==\n";
  r = httpRequest(host, port, "DELETE", "/api/roots/" + std::to_string(root_id));
  check(r.status == 200, "DELETE /api/roots/<id> -> 200");

  std::cout << "\n结果: PASS=" << g_pass << " FAIL=" << g_fail << "\n";
  return g_fail == 0 ? 0 : 1;
}
