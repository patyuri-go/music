// 播放列表接口测试（C++，走真实 HTTP）。
// 用法：先启动 musicd（主 5666 / Jellyfin 9000），再运行本程序。
//
//   ./playlist_test            # 默认 127.0.0.1:5666 / :9000
//   ./playlist_test <host> <port> <jellyfin_port>
//
// 退出码：0 表示全部通过，非 0 表示有失败。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "crow/json.h"

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

// 依次通过 album-artists -> artist -> album -> tracks 找一首曲目的 id。
int findTrackId(const std::string &host, int port) {
  HttpResponse r = httpRequest(host, port, "GET", "/api/album-artists");
  auto artists = crow::json::load(r.body);
  if (!artists || artists.t() != crow::json::type::List || artists.size() == 0) {
    return 0;
  }
  const int artist_id = static_cast<int>(artists[0]["id"].i());
  r = httpRequest(host, port, "GET", "/api/artist/" + std::to_string(artist_id));
  auto artist = crow::json::load(r.body);
  if (!artist || !artist.has("albums") ||
      artist["albums"].t() != crow::json::type::List ||
      artist["albums"].size() == 0) {
    return 0;
  }
  const int album_id = static_cast<int>(artist["albums"][0]["id"].i());
  r = httpRequest(host, port, "GET", "/api/album/" + std::to_string(album_id));
  auto album = crow::json::load(r.body);
  if (!album || !album.has("tracks") ||
      album["tracks"].t() != crow::json::type::List ||
      album["tracks"].size() == 0) {
    return 0;
  }
  return static_cast<int>(album["tracks"][0]["id"].i());
}

// 通过 album-artists -> artist -> album 找第一个专辑 id。
int findAlbumId(const std::string &host, int port) {
  HttpResponse r = httpRequest(host, port, "GET", "/api/album-artists");
  auto artists = crow::json::load(r.body);
  if (!artists || artists.t() != crow::json::type::List || artists.size() == 0) {
    return 0;
  }
  const int artist_id = static_cast<int>(artists[0]["id"].i());
  r = httpRequest(host, port, "GET", "/api/artist/" + std::to_string(artist_id));
  auto artist = crow::json::load(r.body);
  if (!artist || !artist.has("albums") ||
      artist["albums"].t() != crow::json::type::List ||
      artist["albums"].size() == 0) {
    return 0;
  }
  return static_cast<int>(artist["albums"][0]["id"].i());
}

} // namespace

int main(int argc, char **argv) {
  const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
  const int port = argc > 2 ? std::atoi(argv[2]) : 5666;
  const int jf_port = argc > 3 ? std::atoi(argv[3]) : 9000;

  std::cout << "== 主服务播放列表接口 ==\n";

  HttpResponse r = httpRequest(host, port, "POST", "/api/playlists/refresh");
  check(r.status == 200, "POST /api/playlists/refresh -> 200");

  r = httpRequest(host, port, "POST", "/api/playlists",
                  "{\"name\":\"Test Playlist\"}");
  check(r.status == 200, "POST /api/playlists -> 200");
  std::string pl_id;
  {
    auto j = crow::json::load(r.body);
    if (j && j.has("id")) {
      pl_id = j["id"].s();
    }
  }
  check(!pl_id.empty(), "新建歌单返回 id");

  if (pl_id.empty()) {
    std::cout << "无法继续（未创建歌单）\n";
    return 1;
  }

  r = httpRequest(host, port, "GET", "/api/playlists/" + pl_id);
  check(r.status == 200, "GET /api/playlists/<id> -> 200");
  {
    auto j = crow::json::load(r.body);
    check(j && j.has("song_count") && j["song_count"].i() == 0,
          "新歌单 song_count=0");
  }

  r = httpRequest(host, port, "GET", "/api/playlists");
  check(r.status == 200, "GET /api/playlists -> 200");

  const int track_id = findTrackId(host, port);
  if (track_id > 0) {
    r = httpRequest(host, port, "POST", "/api/playlists/" + pl_id + "/songs",
                    "{\"trackId\":" + std::to_string(track_id) + "}");
    check(r.status == 200, "POST /api/playlists/<id>/songs (trackId) -> 200");
    {
      auto j = crow::json::load(r.body);
      check(j && j.has("song_count") && j["song_count"].i() == 1,
            "添加后 song_count=1");
      bool matched = false;
      if (j && j.has("songs") && j["songs"].size() > 0) {
        matched = j["songs"][0]["matched"].b();
      }
      check(matched, "添加的曲目 matched=true");
    }

    r = httpRequest(host, port, "DELETE",
                    "/api/playlists/" + pl_id + "/songs/0");
    check(r.status == 200, "DELETE /api/playlists/<id>/songs/0 -> 200");
    r = httpRequest(host, port, "GET", "/api/playlists/" + pl_id);
    {
      auto j = crow::json::load(r.body);
      check(j && j["song_count"].i() == 0, "删除曲目后 song_count=0");
    }
  } else {
    std::cout << "  SKIP  数据库无曲目，跳过加歌测试\n";
  }

  r = httpRequest(host, port, "DELETE", "/api/playlists/" + pl_id);
  check(r.status == 200, "DELETE /api/playlists/<id> -> 200");
  r = httpRequest(host, port, "GET", "/api/playlists/" + pl_id);
  check(r.status == 404, "删除后 GET /api/playlists/<id> -> 404");

  std::cout << "\n== Jellyfin 播放列表接口 ==\n";

  r = httpRequest(host, jf_port, "POST", "/Playlists?name=JF%20Playlist");
  check(r.status == 200, "POST /Playlists -> 200");
  std::string jf_id;
  {
    auto j = crow::json::load(r.body);
    if (j && j.has("Id")) {
      jf_id = j["Id"].s();
    }
  }
  check(!jf_id.empty(), "Jellyfin 新建歌单返回 Id");

  if (!jf_id.empty() && track_id > 0) {
    const std::string guid = trackGuid(track_id);
    r = httpRequest(host, jf_port, "POST",
                    "/Playlists/" + jf_id + "/Items?ids=" + guid);
    check(r.status == 204, "POST /Playlists/<id>/Items -> 204");

    r = httpRequest(host, jf_port, "GET", "/Playlists/" + jf_id);
    check(r.status == 200, "GET /Playlists/<id> -> 200");
    {
      auto j = crow::json::load(r.body);
      bool found = false;
      if (j && j.has("ItemIds")) {
        for (const auto &x : j["ItemIds"]) {
          if (std::string(x.s()) == guid) {
            found = true;
          }
        }
      }
      check(found, "GetPlaylist ItemIds 含刚添加的曲目");
    }
    {
      auto j = crow::json::load(r.body);
      check(j && j.has("RunTimeTicks") && j["RunTimeTicks"].i() > 0,
            "歌单 RunTimeTicks>0（总时长）");
    }

    r = httpRequest(host, jf_port, "GET", "/Playlists/" + jf_id + "/Items");
    check(r.status == 200, "GET /Playlists/<id>/Items -> 200");

    r = httpRequest(host, jf_port, "DELETE",
                    "/Playlists/" + jf_id + "/Items?entryIds=" + guid);
    check(r.status == 204, "DELETE /Playlists/<id>/Items -> 204");
  } else if (!jf_id.empty()) {
    std::cout << "  SKIP  数据库无曲目，跳过 Jellyfin 加歌测试\n";
  }

  if (!jf_id.empty()) {
    r = httpRequest(host, jf_port, "DELETE", "/Playlists/" + jf_id);
    check(r.status == 204, "DELETE /Playlists/<id> -> 204");
  }

  std::cout << "\n== Jellyfin 播放列表接口（JSON body，模拟客户端）==\n";
  r = httpRequest(host, jf_port, "POST", "/Playlists",
                  "{\"Name\":\"Body Playlist\",\"Ids\":[]}");
  check(r.status == 200, "POST /Playlists (body Name) -> 200");
  std::string body_id;
  {
    auto j = crow::json::load(r.body);
    if (j && j.has("Id")) {
      body_id = j["Id"].s();
    }
  }
  check(!body_id.empty(), "body 新建歌单返回 Id");
  if (!body_id.empty()) {
    if (track_id > 0) {
      const std::string guid = trackGuid(track_id);
      r = httpRequest(host, jf_port, "POST", "/Playlists/" + body_id + "/Items",
                      "{\"Ids\":[\"" + guid + "\"]}");
      check(r.status == 204, "POST /Playlists/<id>/Items (body Ids) -> 204");
      r = httpRequest(host, jf_port, "GET", "/Playlists/" + body_id);
      auto j = crow::json::load(r.body);
      check(j && j.has("ChildCount") && j["ChildCount"].i() == 1,
            "body 加歌后 ChildCount=1");
    }
    r = httpRequest(host, jf_port, "GET", "/Items/" + body_id);
    check(r.status == 200, "GET /Items/<playlistId> -> 200");
    r = httpRequest(host, jf_port, "GET",
                    "/Users/0123456789abcdef0123456789abcdef/Items/" + body_id);
    check(r.status == 200, "GET /Users/<uid>/Items/<playlistId> -> 200");
    r = httpRequest(host, jf_port, "DELETE", "/Playlists/" + body_id);
    check(r.status == 204, "DELETE body 歌单 -> 204");
  }

  std::cout << "\n== 专辑时长 ==\n";
  const int album_id = findAlbumId(host, port);
  if (album_id > 0) {
    char abuf[64];
    std::snprintf(abuf, sizeof(abuf), "%08x%024x", 2, album_id);
    r = httpRequest(host, jf_port, "GET", std::string("/Items/") + abuf);
    auto j = crow::json::load(r.body);
    check(j && j.has("RunTimeTicks") && j["RunTimeTicks"].i() > 0,
          "专辑 RunTimeTicks>0（总时长）");
  } else {
    std::cout << "  SKIP  数据库无专辑，跳过时长测试\n";
  }

  std::cout << "\n== 批量 sync 多线程 ==\n";
  std::vector<int> ids;
  if (album_id > 0) {
    r = httpRequest(host, port, "GET",
                    "/api/album/" + std::to_string(album_id));
    auto j = crow::json::load(r.body);
    if (j && j.has("tracks") && j["tracks"].t() == crow::json::type::List) {
      for (const auto &t : j["tracks"]) {
        ids.push_back(static_cast<int>(t["id"].i()));
        if (ids.size() >= 4) {
          break;
        }
      }
    }
  }
  if (ids.size() >= 2) {
    std::string body = "[";
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (i) {
        body += ",";
      }
      body += "{\"action\":\"sync-from-file\",\"trackId\":" +
              std::to_string(ids[i]) + "}";
    }
    body += "]";
    r = httpRequest(host, port, "POST", "/api/fs/batch", body);
    check(r.status == 200, "POST /api/fs/batch (sync x N) -> 200");
    std::string job_id;
    {
      auto j = crow::json::load(r.body);
      if (j && j.t() == crow::json::type::List && j.size() > 0 &&
          j[0].has("jobId")) {
        job_id = std::to_string(j[0]["jobId"].i());
      }
    }
    check(!job_id.empty(), "批量 sync 返回 jobId");
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
            check(st == "done", "批量 sync job 完成 (state=done)");
          }
        }
      }
      if (!finished) {
        check(false, "批量 sync job 在超时内完成");
      }
    }
  } else {
    std::cout << "  SKIP  曲目不足，跳过批量 sync 测试\n";
  }

  std::cout << "\n结果: PASS=" << g_pass << " FAIL=" << g_fail << "\n";
  return g_fail == 0 ? 0 : 1;
}
