#include "playlist.h"

#include <algorithm>
#include <cstdint>
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
  fs::path dir = config::get().local.playlist_dir;
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir;
}

fs::path filePath(const std::string &id) {
  return dirPath() / (id + ".json");
}

std::string cellText(const db::Row &row, std::size_t index) {
  static const db::Cell kNull{db::Null{}};
  const db::Cell &cell = index < row.size() ? row[index] : kNull;
  if (const auto *value = std::get_if<std::string>(&cell.value)) {
    return *value;
  }
  if (const auto *value = std::get_if<std::int64_t>(&cell.value)) {
    return std::to_string(*value);
  }
  if (const auto *value = std::get_if<double>(&cell.value)) {
    return std::to_string(*value);
  }
  return {};
}

std::int64_t cellInt(const db::Row &row, std::size_t index) {
  static const db::Cell kNull{db::Null{}};
  const db::Cell &cell = index < row.size() ? row[index] : kNull;
  if (const auto *value = std::get_if<std::int64_t>(&cell.value)) {
    return *value;
  }
  if (const auto *value = std::get_if<double>(&cell.value)) {
    return static_cast<std::int64_t>(*value);
  }
  if (const auto *value = std::get_if<std::string>(&cell.value)) {
    try {
      return std::stoll(*value);
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

std::string joinNames(const db::ResultSet &result) {
  std::string out;
  for (const auto &row : result.rows) {
    if (!out.empty()) {
      out += "; ";
    }
    out += cellText(row, 0);
  }
  return out;
}

std::string albumName(db::Session &session, int album_id) {
  const auto result = session.query("SELECT name FROM album WHERE id = ?",
                                    {static_cast<std::int64_t>(album_id)});
  if (result.rows.empty()) {
    return std::string();
  }
  return cellText(result.rows.front(), 0);
}

std::string trackArtistNames(db::Session &session, int track_id) {
  return joinNames(session.query(
      "SELECT a.name FROM artist a JOIN track_artist ta "
      "ON ta.artist_id = a.id WHERE ta.track_id = ? "
      "ORDER BY ta.position",
      {static_cast<std::int64_t>(track_id)}));
}

std::string albumArtistNames(db::Session &session, int album_id) {
  return joinNames(session.query(
      "SELECT a.name FROM artist a JOIN album_artist aa "
      "ON aa.artist_id = a.id WHERE aa.album_id = ? "
      "ORDER BY aa.position",
      {static_cast<std::int64_t>(album_id)}));
}

// 用虚拟路径匹配 DB；命中则补全字段。返回是否命中。
bool matchSong(db::Session &session, Song &s) {
  const auto result = session.query(
      "SELECT id, title, album_id, duration_ms FROM track WHERE path = ?",
      {s.path});
  if (result.rows.empty()) {
    s.matched = false;
    s.track_id = 0;
    return false;
  }
  const auto &row = result.rows.front();
  s.matched = true;
  s.track_id = static_cast<int>(cellInt(row, 0));
  s.title = cellText(row, 1);
  const int album_id = static_cast<int>(cellInt(row, 2));
  s.duration_ms = cellInt(row, 3);
  s.artist = trackArtistNames(session, s.track_id);
  if (album_id > 0) {
    s.album = albumName(session, album_id);
    s.album_artist = albumArtistNames(session, album_id);
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
  db::Session &session = db::mainSession();
  for (auto &kv : g_playlists) {
    for (auto &s : kv.second.songs) {
      matchSong(session, s);
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

bool addPath(const std::string &id, const std::string &path,
             std::string &err) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_playlists.find(id);
  if (it == g_playlists.end()) {
    err = "playlist not found";
    return false;
  }
  if (path.empty()) {
    err = "empty path";
    return false;
  }
  Song s;
  s.path = path;
  matchSong(db::mainSession(), s);
  it->second.songs.push_back(std::move(s));
  saveLocked(it->second);
  return true;
}

bool addTrack(const std::string &id, int track_id, std::string &err) {
  repo::TrackRepo tr(db::mainSession());
  auto track = tr.byId(track_id);
  if (!track) {
    err = "track not found";
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
