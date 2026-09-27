#include "playlist.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

#include "config.h"
#include "crow/json.h"
#include "database.h"
#include "db.h"
#include "storage.h"
#include "SQLiteCpp/Database.h"
#include "SQLiteCpp/Statement.h"

namespace playlist {
namespace {

namespace fs = std::filesystem;

std::mutex g_mutex;
std::map<std::string, Playlist> g_playlists;

std::string makeId() {
  static std::mutex rng_mu;
  static std::mt19937_64 rng(std::random_device{}());
  std::lock_guard<std::mutex> lock(rng_mu);
  char buf[33];
  std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                static_cast<unsigned long long>(rng()),
                static_cast<unsigned long long>(rng()));
  return std::string(buf);
}

fs::path dirPath() {
  fs::path dir = config::get().playlist_dir;
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir;
}

fs::path filePath(const std::string &id) {
  return dirPath() / (id + ".json");
}

std::string text(SQLite::Statement &q, int col) {
  if (q.getColumn(col).isNull()) {
    return std::string();
  }
  return q.getColumn(col).getText();
}

std::string albumName(SQLite::Database &db, int album_id) {
  SQLite::Statement q(db, "SELECT name FROM album WHERE id = ?");
  q.bind(1, album_id);
  if (q.executeStep()) {
    return text(q, 0);
  }
  return std::string();
}

std::string joinNames(SQLite::Statement &q) {
  std::string out;
  while (q.executeStep()) {
    if (!out.empty()) {
      out += "; ";
    }
    out += text(q, 0);
  }
  return out;
}

std::string trackArtistNames(SQLite::Database &db, int track_id) {
  SQLite::Statement q(db,
                      "SELECT a.name FROM artist a JOIN track_artist ta "
                      "ON ta.artist_id = a.id WHERE ta.track_id = ? "
                      "ORDER BY ta.position");
  q.bind(1, track_id);
  return joinNames(q);
}

std::string albumArtistNames(SQLite::Database &db, int album_id) {
  SQLite::Statement q(db,
                      "SELECT a.name FROM artist a JOIN album_artist aa "
                      "ON aa.artist_id = a.id WHERE aa.album_id = ? "
                      "ORDER BY aa.position");
  q.bind(1, album_id);
  return joinNames(q);
}

// 用虚拟路径匹配 DB；命中则补全字段。返回是否命中。
bool matchSong(SQLite::Database &db, Song &s) {
  SQLite::Statement q(db,
                      "SELECT id, title, album_id, duration_ms FROM track "
                      "WHERE path = ?");
  q.bind(1, s.path);
  if (!q.executeStep()) {
    s.matched = false;
    s.track_id = 0;
    return false;
  }
  s.matched = true;
  s.track_id = q.getColumn(0).getInt();
  s.title = text(q, 1);
  const int album_id = q.getColumn(2).getInt();
  s.duration_ms = q.getColumn(3).getInt64();
  s.artist = trackArtistNames(db, s.track_id);
  if (album_id > 0) {
    s.album = albumName(db, album_id);
    s.album_artist = albumArtistNames(db, album_id);
  }
  return true;
}

std::string toJson(const Playlist &p) {
  crow::json::wvalue w;
  w["id"] = p.id;
  w["name"] = p.name;
  crow::json::wvalue::list songs;
  for (const auto &s : p.songs) {
    crow::json::wvalue sw;
    sw["path"] = s.path;
    sw["album"] = s.album;
    sw["album_artist"] = s.album_artist;
    songs.emplace_back(std::move(sw));
  }
  w["songs"] = crow::json::wvalue(songs);
  return w.dump();
}

bool saveLocked(const Playlist &p) {
  std::ofstream out(filePath(p.id), std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << toJson(p);
  return static_cast<bool>(out);
}

Playlist parseFile(const fs::path &file) {
  Playlist p;
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return p;
  }
  std::stringstream buf;
  buf << in.rdbuf();
  auto j = crow::json::load(buf.str());
  if (!j) {
    return p;
  }
  if (j.has("id")) {
    p.id = j["id"].s();
  }
  if (p.id.empty()) {
    p.id = file.stem().string();
  }
  if (j.has("name")) {
    p.name = j["name"].s();
  }
  if (j.has("songs") && j["songs"].t() == crow::json::type::List) {
    for (const auto &sj : j["songs"]) {
      Song s;
      if (sj.has("path")) {
        s.path = sj["path"].s();
      }
      if (sj.has("album")) {
        s.album = sj["album"].s();
      }
      if (sj.has("album_artist")) {
        s.album_artist = sj["album_artist"].s();
      }
      if (!s.path.empty()) {
        p.songs.push_back(std::move(s));
      }
    }
  }
  return p;
}

} // namespace

void loadAll() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_playlists.clear();
  const fs::path dir = dirPath();
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
      continue;
    }
    Playlist p = parseFile(entry.path());
    if (p.id.empty()) {
      continue;
    }
    g_playlists[p.id] = std::move(p);
  }
  SQLite::Database &db = db::conn(config::get().db_path.string());
  for (auto &kv : g_playlists) {
    for (auto &s : kv.second.songs) {
      matchSong(db, s);
    }
  }
}

RefreshResult refresh() {
  loadAll();
  RefreshResult out;
  std::lock_guard<std::mutex> lock(g_mutex);
  out.playlists = static_cast<int>(g_playlists.size());
  for (const auto &kv : g_playlists) {
    for (const auto &s : kv.second.songs) {
      out.songs++;
      if (s.matched) {
        out.matched++;
      } else {
        out.unmatched.push_back(Unmatched{kv.second.name, s});
      }
    }
  }
  return out;
}

std::vector<Playlist> list() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<Playlist> out;
  out.reserve(g_playlists.size());
  for (const auto &kv : g_playlists) {
    out.push_back(kv.second);
  }
  std::sort(out.begin(), out.end(),
            [](const Playlist &a, const Playlist &b) { return a.name < b.name; });
  return out;
}

bool get(const std::string &id, Playlist &out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_playlists.find(id);
  if (it == g_playlists.end()) {
    return false;
  }
  out = it->second;
  return true;
}

Playlist create(const std::string &name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Playlist p;
  p.id = makeId();
  p.name = name;
  g_playlists[p.id] = p;
  saveLocked(p);
  return p;
}

bool remove(const std::string &id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_playlists.find(id);
  if (it == g_playlists.end()) {
    return false;
  }
  g_playlists.erase(it);
  std::error_code ec;
  fs::remove(filePath(id), ec);
  return true;
}

bool rename(const std::string &id, const std::string &name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_playlists.find(id);
  if (it == g_playlists.end()) {
    return false;
  }
  it->second.name = name;
  return saveLocked(it->second);
}

bool addPath(const std::string &id, const std::string &path, std::string *err) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_playlists.find(id);
  if (it == g_playlists.end()) {
    if (err) {
      *err = "playlist not found";
    }
    return false;
  }
  if (path.empty()) {
    if (err) {
      *err = "empty path";
    }
    return false;
  }
  Song s;
  s.path = path;
  SQLite::Database &db = db::conn(config::get().db_path.string());
  matchSong(db, s);
  it->second.songs.push_back(std::move(s));
  saveLocked(it->second);
  return true;
}

bool addTrack(const std::string &id, int track_id, std::string *err) {
  repo::TrackRepo tr(config::get().db_path.string());
  auto track = tr.byId(track_id);
  if (!track) {
    if (err) {
      *err = "track not found";
    }
    return false;
  }
  return addPath(id, track->path, err);
}

bool removeAt(const std::string &id, int index) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_playlists.find(id);
  if (it == g_playlists.end()) {
    return false;
  }
  auto &songs = it->second.songs;
  if (index < 0 || index >= static_cast<int>(songs.size())) {
    return false;
  }
  songs.erase(songs.begin() + index);
  saveLocked(it->second);
  return true;
}

bool removeTrack(const std::string &id, int track_id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_playlists.find(id);
  if (it == g_playlists.end()) {
    return false;
  }
  auto &songs = it->second.songs;
  const auto before = songs.size();
  songs.erase(std::remove_if(songs.begin(), songs.end(),
                             [track_id](const Song &s) {
                               return s.matched && s.track_id == track_id;
                             }),
              songs.end());
  if (songs.size() == before) {
    return false;
  }
  saveLocked(it->second);
  return true;
}

bool clear(const std::string &id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_playlists.find(id);
  if (it == g_playlists.end()) {
    return false;
  }
  it->second.songs.clear();
  saveLocked(it->second);
  return true;
}

int songCount(const Playlist &p) {
  return static_cast<int>(p.songs.size());
}

long long totalDurationMs(const Playlist &p) {
  long long total = 0;
  for (const auto &s : p.songs) {
    if (s.matched) {
      total += s.duration_ms;
    }
  }
  return total;
}

} // namespace playlist
