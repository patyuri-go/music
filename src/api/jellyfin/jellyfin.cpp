#include "jellyfin.h"

#include "config.h"
#include "database.h"
#include "db.h"
#include "dto.h"
#include "http_util.h"
#include "ids.h"
#include "schema.h"
#include "storage.h"
#include "api/playlist/playlist.h"

#include "crow/app.h"
#include "crow/http_request.h"
#include "crow/http_response.h"
#include "crow/json.h"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace jellyfin {
namespace {

namespace fs = std::filesystem;

constexpr const char *kVersion = "10.10.7";

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string trimCopy(const std::string &s) {
  const std::size_t b = s.find_first_not_of(" \t\r\n");
  const std::size_t e = s.find_last_not_of(" \t\r\n");
  return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
}

// 同时按原样与全小写注册路由，兼容 ASP.NET 的大小写不敏感路由。
struct RouteReg {
  crow::SimpleApp *app;
  std::string path;
  std::function<void(crow::DynamicRule &)> apply_methods;
  RouteReg(crow::SimpleApp *a, std::string p) : app(a), path(std::move(p)) {}
  template <typename... M> RouteReg &methods(M... m) {
    apply_methods = [m...](crow::DynamicRule &r) { r.methods(m...); };
    return *this;
  }
  template <typename F> void operator()(F f) {
    // 路由匹配已由 crow::case_insensitive_routing() 全局开启大小写不敏感，
    // 这里只需按原样注册一次。
    auto &r = app->route_dynamic(path);
    if (apply_methods) apply_methods(r);
    r(f);
  }
};

// 查询参数读取：Jellyfin 客户端使用 PascalCase（如 ParentId），
// 而 Crow 的 url_params.get() 大小写敏感，这里统一做大小写不敏感匹配。
std::string qs(const crow::request &req, const char *key) {
  const std::string want = lower(std::string(key));
  for (const auto &k : req.url_params.keys()) {
    if (lower(k) == want) {
      const char *v = req.url_params.get(k);
      return v ? std::string(v) : std::string();
    }
  }
  return std::string();
}

int qsInt(const crow::request &req, const char *key, int def) {
  const std::string v = qs(req, key);
  if (v.empty()) {
    return def;
  }
  try {
    return std::stoi(v);
  } catch (...) {
    return def;
  }
}

bool qsBool(const crow::request &req, const char *key, bool def) {
  const std::string v = qs(req, key);
  if (v.empty()) {
    return def;
  }
  return v == "true" || v == "1" || v == "True";
}

bool listHas(const std::string &csv, const std::string &needle) {
  if (csv.empty()) {
    return false;
  }
  const std::string hay = lower(csv);
  const std::string want = lower(needle);
  std::string::size_type pos = 0;
  while (pos <= hay.size()) {
    const auto comma = hay.find(',', pos);
    const std::string token =
        hay.substr(pos, comma == std::string::npos ? std::string::npos
                                                   : comma - pos);
    if (token == want) {
      return true;
    }
    if (comma == std::string::npos) {
      break;
    }
    pos = comma + 1;
  }
  return false;
}

crow::response json(int code, const crow::json::wvalue &v) {
  return http_util::jsonResponse(code, v.dump());
}

crow::response emptyList() {
  crow::json::wvalue arr;
  arr = crow::json::wvalue::list{};
  return json(200, arr);
}

// ---------------------------------------------------------------- 数据访问

SQLite::Database &db() { return db::conn(config::get().db_path.string()); }

std::string text(SQLite::Statement &q, int col) {
  return q.getColumn(col).isNull() ? std::string() : q.getColumn(col).getText();
}

std::vector<int> parseCsvInts(const std::string &csv) {
  std::vector<int> out;
  std::size_t start = 0;
  while (start < csv.size()) {
    const std::size_t comma = csv.find(',', start);
    const std::string part =
        (comma == std::string::npos) ? csv.substr(start)
                                     : csv.substr(start, comma - start);
    if (!part.empty()) {
      out.push_back(std::stoi(part));
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

int firstArtistId(const std::vector<int> &ids) {
  return ids.empty() ? 0 : ids.front();
}

bool hasArtistId(const std::vector<int> &ids, int id) {
  for (const int x : ids) {
    if (x == id) {
      return true;
    }
  }
  return false;
}

// 读取 track 行：t.* 之后追加一列（按 position 的 artist_id 逗号串）。
repo::Track readTrackRow(SQLite::Statement &q) {
  repo::Track t;
  t.id = q.getColumn(0).getInt();
  t.root_id = q.getColumn(1).getInt();
  t.path = text(q, 2);
  t.title = text(q, 3);
  t.album_id = q.getColumn(4).getInt();
  t.disc_no = q.getColumn(5).getInt();
  t.track_no = q.getColumn(6).getInt();
  t.duration_ms = q.getColumn(7).getInt();
  t.bitrate = q.getColumn(8).getInt();
  t.sample_rate = q.getColumn(9).getInt();
  t.channels = q.getColumn(10).getInt();
  t.format = text(q, 11);
  t.size_bytes = q.getColumn(12).getInt64();
  t.mtime = q.getColumn(13).getInt64();
  t.added_at = text(q, 14);
  t.play_count = q.getColumn(15).getInt();
  t.last_played = text(q, 16);
  t.rating = q.getColumn(17).getInt();
  t.artist_ids = parseCsvInts(text(q, 18));
  return t;
}

struct GenreRow {
  int id = 0;
  std::string name;
};

struct Catalog {
  std::unordered_map<int, repo::Album> albums;
  std::unordered_map<int, std::string> artistNames;
  std::unordered_map<int, int> albumTrackCount;
  std::unordered_map<int, long long> albumDurationMs;
  std::unordered_map<int, int> artistAlbumCount;
  std::unordered_map<int, std::vector<std::pair<std::string, int>>> trackGenres;
  std::unordered_map<int, std::vector<int>> trackArtists;
  std::unordered_map<int, std::vector<int>> albumArtists;
  std::unordered_set<int> tracksWithLyrics;
};

Catalog loadCatalog() {
  Catalog c;
  {
    SQLite::Statement q(db(), "SELECT id,name,year,cover_path FROM album");
    while (q.executeStep()) {
      repo::Album a;
      a.id = q.getColumn(0).getInt();
      a.name = text(q, 1);
      a.year = q.getColumn(2).getInt();
      a.cover_path = text(q, 3);
      c.albums.emplace(a.id, a);
    }
  }
  {
    SQLite::Statement q(db(), "SELECT id,name FROM artist");
    while (q.executeStep()) {
      c.artistNames.emplace(q.getColumn(0).getInt(), text(q, 1));
    }
  }
  {
    SQLite::Statement q(db(), "SELECT album_id, COUNT(*) FROM track WHERE "
                              "album_id > 0 GROUP BY album_id");
    while (q.executeStep()) {
      c.albumTrackCount[q.getColumn(0).getInt()] = q.getColumn(1).getInt();
    }
  }
  {
    SQLite::Statement q(db(), "SELECT album_id, SUM(duration_ms) FROM track WHERE "
                              "album_id > 0 GROUP BY album_id");
    while (q.executeStep()) {
      c.albumDurationMs[q.getColumn(0).getInt()] = q.getColumn(1).getInt64();
    }
  }
  {
    SQLite::Statement q(db(), "SELECT aa.artist_id, COUNT(*) FROM album_artist "
                              "aa GROUP BY aa.artist_id");
    while (q.executeStep()) {
      c.artistAlbumCount[q.getColumn(0).getInt()] = q.getColumn(1).getInt();
    }
  }
  {
    SQLite::Statement q(db(), "SELECT tg.track_id, g.id, g.name FROM track_genre "
                              "tg JOIN genre g ON g.id = tg.genre_id ORDER BY "
                              "g.name");
    while (q.executeStep()) {
      c.trackGenres[q.getColumn(0).getInt()].emplace_back(text(q, 2),
                                                          q.getColumn(1).getInt());
    }
  }
  {
    SQLite::Statement q(db(), "SELECT track_id, artist_id FROM track_artist "
                              "ORDER BY track_id, position");
    while (q.executeStep()) {
      c.trackArtists[q.getColumn(0).getInt()].push_back(q.getColumn(1).getInt());
    }
  }
  {
    SQLite::Statement q(db(), "SELECT album_id, artist_id FROM album_artist "
                              "ORDER BY album_id, position");
    while (q.executeStep()) {
      c.albumArtists[q.getColumn(0).getInt()].push_back(q.getColumn(1).getInt());
    }
  }
  for (auto &kv : c.albums) {
    const auto it = c.albumArtists.find(kv.first);
    if (it != c.albumArtists.end()) {
      kv.second.artist_ids = it->second;
    }
  }
  {
    SQLite::Statement q(db(), "SELECT track_id FROM track_lyrics WHERE text <> ''");
    while (q.executeStep()) {
      c.tracksWithLyrics.insert(q.getColumn(0).getInt());
    }
  }
  return c;
}

std::string albumNameOf(const Catalog &c, int album_id) {
  const auto it = c.albums.find(album_id);
  return it == c.albums.end() ? std::string() : it->second.name;
}

std::string artistNameOf(const Catalog &c, int artist_id) {
  const auto it = c.artistNames.find(artist_id);
  return it == c.artistNames.end() ? std::string() : it->second;
}

std::vector<std::pair<std::string, int>>
artistPairs(const Catalog &c, const std::vector<int> &ids) {
  std::vector<std::pair<std::string, int>> out;
  for (const int id : ids) {
    const auto it = c.artistNames.find(id);
    if (it != c.artistNames.end() && !it->second.empty()) {
      out.emplace_back(it->second, id);
    }
  }
  return out;
}

std::vector<int> trackArtistIds(const Catalog &c, int track_id) {
  const auto it = c.trackArtists.find(track_id);
  return it == c.trackArtists.end() ? std::vector<int>{} : it->second;
}

std::vector<int> albumArtistIds(const Catalog &c, int album_id) {
  const auto it = c.albumArtists.find(album_id);
  return it == c.albumArtists.end() ? std::vector<int>{} : it->second;
}

int albumYearOf(const Catalog &c, int album_id) {
  const auto it = c.albums.find(album_id);
  return it == c.albums.end() ? 0 : it->second.year;
}

long long albumDurationOf(const Catalog &c, int album_id) {
  const auto it = c.albumDurationMs.find(album_id);
  return it == c.albumDurationMs.end() ? 0 : it->second;
}

std::string trackImageTag(const Catalog &c, const repo::Track &t) {
  const auto it = c.albums.find(t.album_id);
  if (it != c.albums.end() && !it->second.cover_path.empty()) {
    return "1";
  }
  return "";
}

std::string artistImageTag(const Catalog &c, int artist_id) {
  for (const auto &kv : c.albums) {
    if (hasArtistId(kv.second.artist_ids, artist_id) &&
        !kv.second.cover_path.empty()) {
      return "1";
    }
  }
  return "";
}

std::vector<repo::Track> allTracks() {
  std::vector<repo::Track> out;
  SQLite::Statement q(db(),
                      "SELECT t.*, (SELECT group_concat(artist_id, ',') FROM "
                      "track_artist WHERE track_id = t.id) FROM track t "
                      "ORDER BY t.album_id, t.disc_no, t.track_no");
  while (q.executeStep()) {
    out.push_back(readTrackRow(q));
  }
  return out;
}

std::vector<repo::Track> tracksByArtist(int artist_id) {
  std::vector<repo::Track> out;
  SQLite::Statement q(
      db(), "SELECT t.*, (SELECT group_concat(artist_id, ',') FROM "
            "track_artist WHERE track_id = t.id) FROM track t JOIN "
            "track_artist ta ON ta.track_id = t.id WHERE ta.artist_id = ? "
            "ORDER BY t.album_id, t.disc_no, t.track_no");
  q.bind(1, artist_id);
  while (q.executeStep()) {
    out.push_back(readTrackRow(q));
  }
  return out;
}

std::vector<repo::Track> tracksByGenre(int genre_id) {
  std::vector<repo::Track> out;
  SQLite::Statement q(
      db(),
      "SELECT t.*, (SELECT group_concat(artist_id, ',') FROM track_artist "
      "WHERE track_id = t.id) FROM track t JOIN track_genre tg ON t.id = "
      "tg.track_id WHERE tg.genre_id = ? ORDER BY t.album_id, t.disc_no, "
      "t.track_no");
  q.bind(1, genre_id);
  while (q.executeStep()) {
    out.push_back(readTrackRow(q));
  }
  return out;
}

std::vector<repo::Album> allAlbums() {
  std::vector<repo::Album> out;
  SQLite::Statement q(
      db(), "SELECT a.id, a.name, a.year, a.cover_path, "
            "(SELECT group_concat(artist_id, ',') FROM album_artist "
            "WHERE album_id = a.id) FROM album a WHERE a.name <> '' "
            "ORDER BY a.name");
  while (q.executeStep()) {
    repo::Album a;
    a.id = q.getColumn(0).getInt();
    a.name = text(q, 1);
    a.year = q.getColumn(2).getInt();
    a.cover_path = text(q, 3);
    a.artist_ids = parseCsvInts(text(q, 4));
    out.push_back(a);
  }
  return out;
}

std::vector<repo::Artist> allArtists() {
  std::vector<repo::Artist> out;
  SQLite::Statement q(db(),
                      "SELECT id,name,sort_name FROM artist WHERE name <> '' "
                      "ORDER BY name");
  while (q.executeStep()) {
    repo::Artist a;
    a.id = q.getColumn(0).getInt();
    a.name = text(q, 1);
    a.sort_name = text(q, 2);
    out.push_back(a);
  }
  return out;
}

std::vector<GenreRow> allGenres() {
  std::vector<GenreRow> out;
  SQLite::Statement q(db(), "SELECT id,name FROM genre ORDER BY name");
  while (q.executeStep()) {
    GenreRow g;
    g.id = q.getColumn(0).getInt();
    g.name = text(q, 1);
    out.push_back(g);
  }
  return out;
}

std::string genreName(int id) {
  SQLite::Statement q(db(), "SELECT name FROM genre WHERE id = ?");
  q.bind(1, id);
  if (q.executeStep()) {
    return text(q, 0);
  }
  return "";
}

std::string coverPathFor(int kind, int id) {
  const std::string path = config::get().db_path.string();
  if (kind == kKindAlbum) {
    repo::AlbumRepo ar(path);
    auto a = ar.byId(id);
    return a ? a->cover_path : std::string();
  }
  if (kind == kKindTrack) {
    repo::TrackRepo tr(path);
    auto t = tr.byId(id);
    if (!t || t->album_id <= 0) {
      return "";
    }
    repo::AlbumRepo ar(path);
    auto a = ar.byId(t->album_id);
    return a ? a->cover_path : std::string();
  }
  if (kind == kKindArtist) {
    repo::AlbumRepo ar(path);
    for (const auto &a : ar.byArtist(id)) {
      if (!a.cover_path.empty()) {
        return a.cover_path;
      }
    }
  }
  return "";
}

// ------------------------------------------------------------------ 排序/分页

struct Sortable {
  std::string sortKey;
  std::string addedAt;
  int year = 0;
  std::string albumKey;
  std::string artistKey;
  int playCount = 0;
  std::string lastPlayed;
  int discNo = 1;
  int trackNo = 0;
  crow::json::wvalue dto;
};

namespace {

enum class SortField {
  SortKey,
  AlbumKey,
  ArtistKey,
  AddedAt,
  Year,
  PlayCount,
  LastPlayed,
  DiscNo,
  TrackNo
};

int compareSortField(const Sortable &a, const Sortable &b, SortField f) {
  switch (f) {
  case SortField::SortKey: {
    const std::string x = lower(a.sortKey);
    const std::string y = lower(b.sortKey);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  case SortField::AlbumKey: {
    const std::string x = lower(a.albumKey);
    const std::string y = lower(b.albumKey);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  case SortField::ArtistKey: {
    const std::string x = lower(a.artistKey);
    const std::string y = lower(b.artistKey);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  case SortField::AddedAt:
    return a.addedAt < b.addedAt ? -1 : (a.addedAt > b.addedAt ? 1 : 0);
  case SortField::Year:
    return a.year < b.year ? -1 : (a.year > b.year ? 1 : 0);
  case SortField::PlayCount:
    return a.playCount < b.playCount ? -1 : (a.playCount > b.playCount ? 1 : 0);
  case SortField::LastPlayed:
    return a.lastPlayed < b.lastPlayed
               ? -1
               : (a.lastPlayed > b.lastPlayed ? 1 : 0);
  case SortField::DiscNo:
    return a.discNo < b.discNo ? -1 : (a.discNo > b.discNo ? 1 : 0);
  case SortField::TrackNo:
    return a.trackNo < b.trackNo ? -1 : (a.trackNo > b.trackNo ? 1 : 0);
  }
  return 0;
}

} // namespace

void sortItems(std::vector<Sortable> &items, const std::string &sortBy,
               const std::string &order) {
  // Jellyfin sends a comma separated list of sort keys. Apply every key we
  // recognise, in order, as a tie-breaker chain. Unknown keys are ignored so
  // that the order produced by the database query is preserved.
  std::vector<SortField> fields;
  bool random = false;
  std::size_t start = 0;
  while (start <= sortBy.size()) {
    const std::size_t comma = sortBy.find(',', start);
    std::string tok =
        (comma == std::string::npos) ? sortBy.substr(start)
                                     : sortBy.substr(start, comma - start);
    tok = lower(trimCopy(tok));
    if (tok == "random") {
      random = true;
    } else if (tok == "name" || tok == "sortname") {
      fields.push_back(SortField::SortKey);
    } else if (tok == "album") {
      fields.push_back(SortField::AlbumKey);
    } else if (tok == "albumartist" || tok == "artist") {
      fields.push_back(SortField::ArtistKey);
    } else if (tok == "datecreated" || tok == "datelastcontentadded" ||
               tok == "datelastmediaadded") {
      fields.push_back(SortField::AddedAt);
    } else if (tok == "productionyear") {
      fields.push_back(SortField::Year);
    } else if (tok == "playcount") {
      fields.push_back(SortField::PlayCount);
    } else if (tok == "dateplayed" || tok == "datelastplayed") {
      fields.push_back(SortField::LastPlayed);
    } else if (tok == "parentindexnumber") {
      fields.push_back(SortField::DiscNo);
    } else if (tok == "indexnumber") {
      fields.push_back(SortField::TrackNo);
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  if (random) {
    std::mt19937 rng(std::random_device{}());
    std::shuffle(items.begin(), items.end(), rng);
    return;
  }
  if (fields.empty()) {
    return;
  }
  std::stable_sort(items.begin(), items.end(),
                   [&fields](const Sortable &a, const Sortable &b) {
                     for (const SortField f : fields) {
                       const int c = compareSortField(a, b, f);
                       if (c != 0) {
                         return c < 0;
                       }
                     }
                     return false;
                   });
  if (order == "Descending") {
    std::reverse(items.begin(), items.end());
  }
}

crow::response respondItems(std::vector<Sortable> &items,
                            const std::string &sortBy,
                            const std::string &order, int startIndex,
                            int limit) {
  sortItems(items, sortBy, order);
  const int total = static_cast<int>(items.size());
  int from = startIndex < 0 ? 0 : startIndex;
  if (from > total) {
    from = total;
  }
  int to = total;
  if (limit > 0 && from + limit < total) {
    to = from + limit;
  }
  std::vector<crow::json::wvalue> out;
  for (int i = from; i < to; ++i) {
    out.push_back(std::move(items[i].dto));
  }
  return json(200, queryResult(std::move(out), total, from));
}

Sortable makeTrack(const Catalog &c, const repo::Track &t) {
  Sortable s;
  s.sortKey = t.title;
  s.addedAt = t.added_at;
  s.year = albumYearOf(c, t.album_id);
  static const std::vector<std::pair<std::string, int>> kNoGenres;
  const auto git = c.trackGenres.find(t.id);
  const auto &genres = git == c.trackGenres.end() ? kNoGenres : git->second;
  const std::string albumName = albumNameOf(c, t.album_id);
  const auto trackArtists = artistPairs(c, trackArtistIds(c, t.id));
  const auto albumArtists = artistPairs(c, albumArtistIds(c, t.album_id));
  s.albumKey = albumName;
  s.artistKey = trackArtists.empty() ? std::string() : trackArtists.front().first;
  s.playCount = t.play_count;
  s.lastPlayed = t.last_played;
  s.discNo = t.disc_no;
  s.trackNo = t.track_no;
  s.dto = trackItem(t, albumName, trackArtists, albumArtists, s.year,
                    trackImageTag(c, t), genres,
                    c.tracksWithLyrics.count(t.id) != 0);
  return s;
}

Sortable makeAlbum(const Catalog &c, const repo::Album &a) {
  Sortable s;
  s.sortKey = a.name;
  s.year = a.year;
  int count = 0;
  const auto it = c.albumTrackCount.find(a.id);
  if (it != c.albumTrackCount.end()) {
    count = it->second;
  }
  s.dto = albumItem(a, artistPairs(c, albumArtistIds(c, a.id)), count,
                    a.cover_path.empty() ? "" : "1",
                    albumDurationOf(c, a.id) * 10000LL);
  return s;
}

Sortable makeArtist(const Catalog &c, const repo::Artist &a) {
  Sortable s;
  s.sortKey = a.sort_name.empty() ? a.name : a.sort_name;
  int count = 0;
  const auto it = c.artistAlbumCount.find(a.id);
  if (it != c.artistAlbumCount.end()) {
    count = it->second;
  }
  s.dto = artistItem(a, count, artistImageTag(c, a.id));
  return s;
}

Sortable makeGenre(int id, const std::string &name) {
  Sortable s;
  s.sortKey = name;
  s.dto = genreItem(id, name);
  return s;
}

Sortable makePlaylist(const playlist::Playlist &p) {
  int matched = 0;
  long long totalMs = 0;
  for (const auto &song : p.songs) {
    if (song.matched) {
      ++matched;
      totalMs += song.duration_ms;
    }
  }
  Sortable s;
  s.sortKey = p.name;
  crow::json::wvalue it;
  it["Name"] = p.name;
  it["ServerId"] = serverId();
  it["Id"] = p.id;
  it["Type"] = "Playlist";
  it["MediaType"] = "Audio";
  it["IsFolder"] = true;
  it["ChildCount"] = matched;
  if (totalMs > 0) {
    it["RunTimeTicks"] = totalMs * 10000LL;
    it["CumulativeRunTimeTicks"] = totalMs * 10000LL;
  }
  s.dto = std::move(it);
  return s;
}

// 歌单 DTO（与 Jellyfin GetPlaylist 一致）。
crow::json::wvalue playlistDto(const playlist::Playlist &p) {
  crow::json::wvalue::list ids;
  int matched = 0;
  long long totalMs = 0;
  for (const auto &song : p.songs) {
    if (song.matched) {
      ids.emplace_back(makeId(kKindTrack, song.track_id));
      ++matched;
      totalMs += song.duration_ms;
    }
  }
  crow::json::wvalue r;
  r["Name"] = p.name;
  r["ServerId"] = serverId();
  r["Id"] = p.id;
  r["Type"] = "Playlist";
  r["MediaType"] = "Audio";
  r["IsFolder"] = true;
  r["ItemIds"] = crow::json::wvalue(ids);
  r["ChildCount"] = matched;
  if (totalMs > 0) {
    r["RunTimeTicks"] = totalMs * 10000LL;
    r["CumulativeRunTimeTicks"] = totalMs * 10000LL;
  }
  return r;
}

// ---------------------------------------------------------------- 视图/系统

crow::json::wvalue musicView() {
  crow::json::wvalue v;
  v["Name"] = "Music";
  v["ServerId"] = serverId();
  v["Id"] = musicViewId();
  v["Type"] = "UserView";
  v["CollectionType"] = "music";
  v["IsFolder"] = true;
  return v;
}

crow::json::wvalue playlistsView() {
  crow::json::wvalue v;
  v["Name"] = "Playlists";
  v["ServerId"] = serverId();
  v["Id"] = playlistsViewId();
  v["Type"] = "UserView";
  v["CollectionType"] = "playlists";
  v["IsFolder"] = true;
  return v;
}

crow::response userViews() {
  std::vector<crow::json::wvalue> items;
  items.push_back(musicView());
  items.push_back(playlistsView());
  return json(200, queryResult(std::move(items), 2, 0));
}

crow::json::wvalue publicSystemInfo() {
  crow::json::wvalue r;
  r["LocalAddress"] = "127.0.0.1";
  r["ServerName"] = serverName();
  r["Version"] = kVersion;
  r["ProductName"] = "Jellyfin Server";
  r["OperatingSystem"] = "Linux";
  r["Id"] = serverId();
  r["StartupWizardCompleted"] = true;
  return r;
}

crow::json::wvalue systemInfo() {
  crow::json::wvalue r = publicSystemInfo();
  r["OperatingSystemDisplayName"] = "Linux";
  r["PackageName"] = "";
  r["HasPendingRestart"] = false;
  r["IsShuttingDown"] = false;
  r["SupportsLibraryMonitor"] = false;
  r["WebSocketPortNumber"] = 0;
  r["CanSelfRestart"] = false;
  r["CanLaunchWebBrowser"] = false;
  r["ProgramDataPath"] = "";
  r["WebPath"] = "/web";
  r["ItemsByNamePath"] = "";
  r["CachePath"] = "";
  r["LogPath"] = "";
  r["InternalMetadataPath"] = "";
  r["TranscodingTempPath"] = "";
  r["HasUpdateAvailable"] = false;
  r["EncoderLocation"] = "";
  r["SystemArchitecture"] = "X64";
  r["CompletedInstallations"] =
      crow::json::wvalue(crow::json::wvalue::list{});
  r["CastReceiverApplications"] =
      crow::json::wvalue(crow::json::wvalue::list{});
  return r;
}

crow::json::wvalue brandingOptions() {
  crow::json::wvalue r;
  r["LoginDisclaimer"] = "";
  r["CustomCss"] = "";
  r["SplashscreenEnabled"] = false;
  return r;
}

crow::json::wvalue displayPreferences(const std::string &id) {
  crow::json::wvalue r;
  r["Id"] = id;
  r["ViewType"] = "";
  r["SortBy"] = "";
  r["IndexBy"] = "";
  r["RememberIndexing"] = false;
  r["PrimaryImageHeight"] = 250;
  r["PrimaryImageWidth"] = 250;
  crow::json::wvalue prefs;
  prefs["client"] = "emby";
  r["CustomPrefs"] = std::move(prefs);
  r["ScrollDirection"] = "Horizontal";
  r["ShowBackdrop"] = true;
  r["RememberSorting"] = false;
  r["SortOrder"] = "Ascending";
  r["ShowSidebar"] = false;
  r["Client"] = "";
  return r;
}

// ------------------------------------------------------------------- 核心接口

std::vector<std::string> splitCsv(const std::string &s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t comma = s.find(',', start);
    std::string part = (comma == std::string::npos) ? s.substr(start)
                                                    : s.substr(start, comma - start);
    if (!part.empty()) out.push_back(part);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

crow::response handleItems(const crow::request &req) {
  const std::string parent = qs(req, "parentId");
  const std::string include = qs(req, "includeItemTypes");
  const std::string search = lower(qs(req, "searchTerm"));
  const int startIndex = qsInt(req, "startIndex", 0);
  const int limit = qsInt(req, "limit", 0);
  const std::string sortBy = qs(req, "sortBy");
  const std::string order = qs(req, "sortOrder");
  const bool isPlayed = qsBool(req, "isPlayed", false);
  const std::vector<std::string> wantGenres = splitCsv(qs(req, "genres"));
  const std::vector<std::string> wantGenreIds = splitCsv(qs(req, "genreIds"));
  const std::vector<std::string> wantYears = splitCsv(qs(req, "years"));
  const std::vector<std::string> wantAlbumIds = splitCsv(qs(req, "albumIds"));
  const std::vector<std::string> wantArtistIds = splitCsv(qs(req, "artistIds"));
  const std::vector<std::string> wantAlbumArtistIds =
      splitCsv(qs(req, "albumArtistIds"));
  const std::vector<std::string> wantContribArtists =
      splitCsv(qs(req, "contributingArtistIds"));
  const std::string filters = qs(req, "filters");
  const bool wantPlayed = filters.find("IsPlayed") != std::string::npos;
  const bool wantUnplayed = filters.find("IsUnplayed") != std::string::npos;
  const std::string nameStartsWith = lower(qs(req, "nameStartsWith"));

  int pk = 0;
  int pid = 0;
  const bool hasParent = parseId(parent, pk, pid);

  Catalog c = loadCatalog();

  enum class Mode { Tracks, Albums, Artists, Genres, Playlists };
  Mode mode = Mode::Albums;
  bool explicitMode = false;
  if (listHas(include, "MusicArtist")) {
    mode = Mode::Artists;
    explicitMode = true;
  } else if (listHas(include, "MusicAlbum")) {
    mode = Mode::Albums;
    explicitMode = true;
  } else if (listHas(include, "MusicGenre")) {
    mode = Mode::Genres;
    explicitMode = true;
  } else if (listHas(include, "Playlist")) {
    mode = Mode::Playlists;
    explicitMode = true;
  } else if (listHas(include, "Audio") || listHas(include, "MusicVideo")) {
    mode = Mode::Tracks;
    explicitMode = true;
  }
  if (!explicitMode && hasParent) {
    if (pk == kKindAlbum || pk == kKindGenre) {
      mode = Mode::Tracks;
    } else if (pk == kKindArtist) {
      mode = Mode::Albums;
    }
  }

  std::vector<Sortable> items;
  if (mode == Mode::Tracks) {
    std::vector<repo::Track> tracks;
    if (hasParent && pk == kKindAlbum) {
      repo::TrackRepo tr(config::get().db_path.string());
      tracks = tr.byAlbum(pid);
    } else if (hasParent && pk == kKindArtist) {
      tracks = tracksByArtist(pid);
    } else if (hasParent && pk == kKindGenre) {
      tracks = tracksByGenre(pid);
    } else {
      tracks = allTracks();
    }
    for (auto &t : tracks) {
      if (!search.empty() && lower(t.title).find(search) == std::string::npos) {
        continue;
      }
      if (isPlayed && t.play_count <= 0) {
        continue;
      }
      if (!nameStartsWith.empty() && lower(t.title).rfind(nameStartsWith, 0) != 0) {
        continue;
      }
      if (wantPlayed && t.play_count <= 0) {
        continue;
      }
      if (wantUnplayed && t.play_count > 0) {
        continue;
      }
      if (!wantAlbumIds.empty()) {
        bool ok = false;
        for (auto &s : wantAlbumIds) {
          int k = 0, v = 0;
          if (parseId(s, k, v) && k == kKindAlbum && v == t.album_id) {
            ok = true;
            break;
          }
        }
        if (!ok) {
          continue;
        }
      }
      if (!wantArtistIds.empty() || !wantContribArtists.empty()) {
        bool ok = false;
        const auto &list = wantArtistIds.empty() ? wantContribArtists : wantArtistIds;
        const std::vector<int> aaids = albumArtistIds(c, t.album_id);
        for (auto &s : list) {
          int k = 0, v = 0;
          if (parseId(s, k, v) && k == kKindArtist &&
              (hasArtistId(t.artist_ids, v) || hasArtistId(aaids, v))) {
            ok = true;
            break;
          }
        }
        if (!ok) {
          continue;
        }
      }
      if (!wantAlbumArtistIds.empty()) {
        bool ok = false;
        const std::vector<int> aaids = albumArtistIds(c, t.album_id);
        for (auto &s : wantAlbumArtistIds) {
          int k = 0, v = 0;
          if (parseId(s, k, v) && k == kKindArtist && hasArtistId(aaids, v)) {
            ok = true;
            break;
          }
        }
        if (!ok) {
          continue;
        }
      }
      if (!wantYears.empty()) {
        const int y = albumYearOf(c, t.album_id);
        bool ok = false;
        for (auto &s : wantYears) {
          if (s == std::to_string(y)) {
            ok = true;
            break;
          }
        }
        if (!ok) {
          continue;
        }
      }
      if (!wantGenres.empty() || !wantGenreIds.empty()) {
        bool ok = false;
        const auto git = c.trackGenres.find(t.id);
        if (git != c.trackGenres.end()) {
          for (auto &g : git->second) {
            for (auto &want : wantGenres) {
              if (lower(g.first) == lower(want)) {
                ok = true;
                break;
              }
            }
            if (ok) {
              break;
            }
            for (auto &s : wantGenreIds) {
              int k = 0, v = 0;
              if (parseId(s, k, v) && k == kKindGenre && v == g.second) {
                ok = true;
                break;
              }
            }
            if (ok) {
              break;
            }
          }
        }
        if (!ok) {
          continue;
        }
      }
      items.push_back(makeTrack(c, t));
    }
  } else if (mode == Mode::Albums) {
    std::vector<repo::Album> albums;
    if (hasParent && pk == kKindArtist) {
      repo::AlbumRepo ar(config::get().db_path.string());
      albums = ar.byArtist(pid);
    } else {
      albums = allAlbums();
    }
    for (auto &a : albums) {
      if (!search.empty() && lower(a.name).find(search) == std::string::npos) {
        continue;
      }
      if (!wantArtistIds.empty() || !wantAlbumArtistIds.empty()) {
        bool ok = false;
        const auto &list =
            wantArtistIds.empty() ? wantAlbumArtistIds : wantArtistIds;
        for (auto &s : list) {
          int k = 0, v = 0;
          if (parseId(s, k, v) && k == kKindArtist && hasArtistId(a.artist_ids, v)) {
            ok = true;
            break;
          }
        }
        if (!ok) {
          continue;
        }
      }
      items.push_back(makeAlbum(c, a));
    }
  } else if (mode == Mode::Artists) {
    for (auto &a : allArtists()) {
      if (!search.empty() && lower(a.name).find(search) == std::string::npos) {
        continue;
      }
      items.push_back(makeArtist(c, a));
    }
  } else if (mode == Mode::Genres) {
    for (auto &g : allGenres()) {
      if (!search.empty() && lower(g.name).find(search) == std::string::npos) {
        continue;
      }
      items.push_back(makeGenre(g.id, g.name));
    }
  } else {
    for (const auto &p : playlist::list()) {
      if (!search.empty() && lower(p.name).find(search) == std::string::npos) {
        continue;
      }
      items.push_back(makePlaylist(p));
    }
  }
  return respondItems(items, sortBy, order, startIndex, limit);
}

crow::response handleLatest(const crow::request &req) {
  const std::string include = qs(req, "includeItemTypes");
  const int limit = qsInt(req, "limit", 20);
  Catalog c = loadCatalog();
  std::vector<crow::json::wvalue> out;
  if (listHas(include, "Audio")) {
    repo::TrackRepo tr(config::get().db_path.string());
    for (auto &t : tr.recent(0, limit > 0 ? limit : 20)) {
      out.push_back(makeTrack(c, t).dto);
    }
  } else {
    auto albums = allAlbums();
    std::sort(albums.begin(), albums.end(),
              [](const repo::Album &a, const repo::Album &b) {
                return a.id > b.id;
              });
    int n = 0;
    for (auto &a : albums) {
      if (limit > 0 && n >= limit) {
        break;
      }
      out.push_back(makeAlbum(c, a).dto);
      ++n;
    }
  }
  crow::json::wvalue arr;
  arr = std::move(out);
  return json(200, arr);
}

crow::response handleItem(const std::string &idstr) {
  {
    playlist::Playlist p;
    if (playlist::get(idstr, p)) {
      return json(200, playlistDto(p));
    }
  }
  int kind = 0;
  int id = 0;
  if (!parseId(idstr, kind, id)) {
    return http_util::jsonResponse(404, "{}");
  }
  Catalog c = loadCatalog();
  crow::json::wvalue dto;
  bool found = false;
  const std::string path = config::get().db_path.string();
  if (kind == kKindTrack) {
    repo::TrackRepo tr(path);
    auto t = tr.byId(id);
    if (t) {
      static const std::vector<std::pair<std::string, int>> kNoGenres;
      const auto git = c.trackGenres.find(t->id);
      const auto &genres = git == c.trackGenres.end() ? kNoGenres : git->second;
      dto = trackItem(*t, albumNameOf(c, t->album_id),
                      artistPairs(c, trackArtistIds(c, t->id)),
                      artistPairs(c, albumArtistIds(c, t->album_id)),
                      albumYearOf(c, t->album_id), trackImageTag(c, *t), genres,
                      c.tracksWithLyrics.count(t->id) != 0);
      found = true;
    }
  } else if (kind == kKindAlbum) {
    repo::AlbumRepo ar(path);
    auto a = ar.byId(id);
    if (a) {
      int count = 0;
      const auto it = c.albumTrackCount.find(id);
      if (it != c.albumTrackCount.end()) {
        count = it->second;
      }
      dto = albumItem(*a, artistPairs(c, albumArtistIds(c, id)), count,
                      a->cover_path.empty() ? "" : "1",
                      albumDurationOf(c, id) * 10000LL);
      found = true;
    }
  } else if (kind == kKindArtist) {
    repo::ArtistRepo ar(path);
    auto a = ar.byId(id);
    if (a) {
      int count = 0;
      const auto it = c.artistAlbumCount.find(id);
      if (it != c.artistAlbumCount.end()) {
        count = it->second;
      }
      dto = artistItem(*a, count, artistImageTag(c, id));
      found = true;
    }
  } else if (kind == kKindGenre) {
    const std::string name = genreName(id);
    if (!name.empty()) {
      dto = genreItem(id, name);
      found = true;
    }
  } else if (kind == kKindView) {
    dto = musicView();
    found = true;
  }
  if (!found) {
    return http_util::jsonResponse(404, "{}");
  }
  return json(200, dto);
}

crow::response handleImage(const std::string &idstr) {
  // 歌单封面：每次请求随机返回歌单内一首已匹配曲目的专辑封面。
  playlist::Playlist p;
  if (playlist::get(idstr, p)) {
    std::vector<std::string> covers;
    for (const auto &s : p.songs) {
      if (!s.matched) {
        continue;
      }
      std::string cover = coverPathFor(kKindTrack, s.track_id);
      if (!cover.empty()) {
        covers.push_back(std::move(cover));
      }
    }
    if (covers.empty()) {
      return http_util::jsonResponse(404, "{}");
    }
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<std::size_t> dist(0, covers.size() - 1);
    return http_util::serveImage(covers[dist(rng)]);
  }
  int kind = 0;
  int id = 0;
  if (!parseId(idstr, kind, id)) {
    return http_util::jsonResponse(404, "{}");
  }
  const std::string cover = coverPathFor(kind, id);
  if (cover.empty()) {
    return http_util::jsonResponse(404, "{}");
  }
  return http_util::serveImage(cover);
}

crow::response handleStream(const crow::request &req,
                            const std::string &idstr) {
  int kind = 0;
  int id = 0;
  if (!parseId(idstr, kind, id) || kind != kKindTrack) {
    return http_util::jsonResponse(404, "{}");
  }
  repo::TrackRepo tr(config::get().db_path.string());
  auto t = tr.byId(id);
  if (!t) {
    return http_util::jsonResponse(404, "{}");
  }
  return http_util::serveResource(storage::resolve(t->path),
                                  req.get_header_value("Range"));
}

/// 解析单个 LRC 时间标签（mm:ss、mm:ss.xx 或 mm:ss:xx）为毫秒。
bool parseLrcTimestamp(const std::string &s, long long &ms) {
  // Split the timestamp by ':'. Supported forms:
  //   [mm:ss]  [mm:ss.xx]  [mm:ss:xx]  [h:mm:ss]  [h:mm:ss.xx]
  std::vector<std::string> parts;
  {
    std::string cur;
    for (const char ch : s) {
      if (ch == ':') {
        parts.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(ch);
      }
    }
    parts.push_back(cur);
  }
  if (parts.size() < 2 || parts.size() > 4) {
    return false;
  }
  for (const auto &p : parts) {
    if (p.empty()) {
      return false;
    }
  }
  // Optional fractional part on the last component (after '.' or ',').
  long long frac_ms = 0;
  {
    std::string &last = parts.back();
    const std::size_t dot = last.find_first_of(".,");
    if (dot != std::string::npos) {
      std::string digits;
      for (std::size_t i = dot + 1; i < last.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(last[i]))) {
          digits.push_back(last[i]);
        }
      }
      if (!digits.empty()) {
        frac_ms = static_cast<long long>(
            std::atof(("0." + digits).c_str()) * 1000.0 + 0.5);
      }
      last = last.substr(0, dot);
    }
  }
  const auto toInt = [](const std::string &p, long long &out) -> bool {
    if (p.empty()) {
      return false;
    }
    out = 0;
    for (const char c : p) {
      if (c < '0' || c > '9') {
        return false;
      }
      out = out * 10 + (c - '0');
    }
    return true;
  };
  long long a = 0;
  long long b = 0;
  long long c = 0;
  if (parts.size() == 2) {
    // mm:ss
    if (!toInt(parts[0], a) || !toInt(parts[1], b)) {
      return false;
    }
    ms = (a * 60 + b) * 1000 + frac_ms;
    return true;
  }
  if (!toInt(parts[0], a) || !toInt(parts[1], b) || !toInt(parts[2], c)) {
    return false;
  }
  if (parts.size() == 3) {
    if (parts[0].size() <= 1) {
      // h:mm:ss
      ms = (a * 3600 + b * 60 + c) * 1000 + frac_ms;
    } else {
      // mm:ss:cc (legacy centisecond form)
      ms = (a * 60 + b) * 1000 + c * 10 + frac_ms;
    }
    return true;
  }
  // h:mm:ss:cc
  long long d = 0;
  if (!toInt(parts[3], d)) {
    return false;
  }
  ms = (a * 3600 + b * 60 + c) * 1000 + d * 10 + frac_ms;
  return true;
}

crow::json::wvalue buildLyricDto(const repo::Track &t,
                                 const std::string &lyrics, bool synced,
                                 const std::string &artist,
                                 const std::string &album) {
  crow::json::wvalue r;
  crow::json::wvalue::list lines;
  std::string by;
  std::string version;
  long long offset_ms = 0;

  std::size_t pos = 0;
  while (pos <= lyrics.size()) {
    const std::size_t nl = lyrics.find('\n', pos);
    std::string raw = lyrics.substr(
        pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? lyrics.size() + 1 : nl + 1;
    if (!raw.empty() && raw.back() == '\r') {
      raw.pop_back();
    }

    std::vector<long long> starts;
    std::size_t i = 0;
    while (i < raw.size() && raw[i] == '[') {
      const std::size_t close = raw.find(']', i);
      if (close == std::string::npos) {
        break;
      }
      const std::string tag = raw.substr(i + 1, close - i - 1);
      long long ms = 0;
      if (parseLrcTimestamp(tag, ms)) {
        starts.push_back(ms);
      } else {
        const auto c2 = tag.find(':');
        if (c2 != std::string::npos) {
          const std::string key = lower(tag.substr(0, c2));
          const std::string val = tag.substr(c2 + 1);
          if (key == "by") {
            by = val;
          } else if (key == "ve") {
            version = val;
          } else if (key == "offset") {
            offset_ms = std::atoll(val.c_str());
          }
        }
      }
      i = close + 1;
    }

    const std::string text = raw.substr(i);
    if (text.empty()) {
      continue;
    }
    if (starts.empty()) {
      crow::json::wvalue line;
      line["Text"] = text;
      lines.push_back(std::move(line));
    } else {
      for (const long long ms : starts) {
        crow::json::wvalue line;
        line["Text"] = text;
        line["Start"] = (ms + offset_ms) * 10000LL;
        lines.push_back(std::move(line));
      }
    }
  }

  crow::json::wvalue meta;
  meta["Title"] = t.title;
  if (!artist.empty()) {
    meta["Artist"] = artist;
  }
  if (!album.empty()) {
    meta["Album"] = album;
  }
  if (!by.empty()) {
    meta["By"] = by;
  }
  if (!version.empty()) {
    meta["Version"] = version;
  }
  meta["Length"] = static_cast<long long>(t.duration_ms) * 10000LL;
  meta["IsSynced"] = synced;
  r["Metadata"] = std::move(meta);
  r["Lyrics"] = crow::json::wvalue(lines);
  return r;
}

crow::response handleLyrics(const std::string &idstr) {
  int kind = 0;
  int id = 0;
  if (!parseId(idstr, kind, id) || kind != kKindTrack) {
    return http_util::jsonResponse(404, "{}");
  }
  repo::TrackRepo tr(config::get().db_path.string());
  auto t = tr.byId(id);
  if (!t) {
    return http_util::jsonResponse(404, "{}");
  }
  std::string lyrics;
  bool synced = false;
  {
    SQLite::Statement q(db(), schema::sql::kTrackLyricsByTrackId);
    q.bind(1, id);
    if (q.executeStep()) {
      lyrics = text(q, 0);
      synced = q.getColumn(1).getInt() != 0;
    }
  }
  if (lyrics.empty()) {
    return http_util::jsonResponse(404, "{}");
  }
  Catalog c = loadCatalog();
  const std::vector<int> artists = trackArtistIds(c, t->id);
  const std::string artist =
      artists.empty() ? std::string() : artistNameOf(c, artists.front());
  return json(200, buildLyricDto(*t, lyrics, synced, artist,
                                 albumNameOf(c, t->album_id)));
}

crow::response handleArtists(const crow::request &req,
                             bool album_only = false) {
  const std::string search = lower(qs(req, "searchTerm"));
  const int startIndex = qsInt(req, "startIndex", 0);
  const int limit = qsInt(req, "limit", 0);
  const std::string sortBy = qs(req, "sortBy");
  const std::string order = qs(req, "sortOrder");
  Catalog c = loadCatalog();
  std::vector<Sortable> items;
  for (auto &a : allArtists()) {
    // AlbumArtists must not include plain (track-only) artists.
    if (album_only &&
        c.artistAlbumCount.find(a.id) == c.artistAlbumCount.end()) {
      continue;
    }
    if (!search.empty() && lower(a.name).find(search) == std::string::npos) {
      continue;
    }
    items.push_back(makeArtist(c, a));
  }
  return respondItems(items, sortBy, order, startIndex, limit);
}

crow::response handleGenres(const crow::request &req) {
  const std::string search = lower(qs(req, "searchTerm"));
  const int startIndex = qsInt(req, "startIndex", 0);
  const int limit = qsInt(req, "limit", 0);
  std::vector<Sortable> items;
  for (auto &g : allGenres()) {
    if (!search.empty() && lower(g.name).find(search) == std::string::npos) {
      continue;
    }
    items.push_back(makeGenre(g.id, g.name));
  }
  return respondItems(items, "", "Ascending", startIndex, limit);
}

crow::response handleMusicGenreByName(const std::string &name) {
  const std::string want = lower(name);
  for (auto &g : allGenres()) {
    if (lower(g.name) == want) {
      return json(200, genreItem(g.id, g.name));
    }
  }
  return http_util::jsonResponse(404, "{}");
}

// ------------------------------------------------------------ 只读扩展接口

// 播放信息：返回单个 MediaSource，供客户端直接播放原始文件。
crow::response handlePlaybackInfo(const crow::request &, const std::string &idstr) {
  int kind = 0, id = 0;
  if (!parseId(idstr, kind, id) || kind != kKindTrack) {
    return http_util::jsonResponse(404, "{}");
  }
  repo::TrackRepo tr(config::get().db_path.string());
  auto t = tr.byId(id);
  if (!t) {
    return http_util::jsonResponse(404, "{}");
  }
  const std::string url =
      "/Audio/" + idstr + "/stream?static=true&mediaSourceId=" + idstr;
  const long long bitrate_bps = static_cast<long long>(t->bitrate) * 1000LL;
  crow::json::wvalue src;
  src["Id"] = idstr;
  src["Name"] = t->title;
  src["Path"] = t->path;
  src["Container"] = t->format;
  src["Size"] = static_cast<long long>(t->size_bytes);
  src["Bitrate"] = bitrate_bps;
  src["RunTimeTicks"] = static_cast<long long>(t->duration_ms) * 10000LL;
  src["SupportsDirectPlay"] = true;
  src["SupportsDirectStream"] = true;
  src["SupportsTranscoding"] = false;
  src["IsRemote"] = false;
  src["Type"] = "Default";
  src["DirectStreamUrl"] = url;
  src["TranscodingUrl"] = url;

  crow::json::wvalue stream;
  stream["Type"] = "Audio";
  stream["Codec"] = t->format;
  stream["Index"] = 0;
  stream["IsDefault"] = true;
  stream["Channels"] = t->channels;
  stream["SampleRate"] = t->sample_rate;
  stream["BitRate"] = bitrate_bps;
  crow::json::wvalue::list streams;
  streams.emplace_back(std::move(stream));
  src["MediaStreams"] = crow::json::wvalue(streams);

  crow::json::wvalue::list sources;
  sources.emplace_back(std::move(src));
  crow::json::wvalue r;
  r["MediaSources"] = crow::json::wvalue(sources);
  r["PlaySessionId"] = "musicd-session";
  return json(200, r);
}

// 仅支持 Primary 图片，其它图片类型返回 404。
crow::response handleImageType(const std::string &idstr, const std::string &imageType,
                               const std::string &) {
  if (lower(imageType) != "primary") {
    return http_util::jsonResponse(404, "{}");
  }
  return handleImage(idstr);
}

// 相似条目：同专辑 + 同流派的其它曲目。
crow::response handleSimilar(const std::string &idstr) {
  std::vector<Sortable> items;
  int kind = 0, id = 0;
  if (parseId(idstr, kind, id) && kind == kKindTrack) {
    repo::TrackRepo tr(config::get().db_path.string());
    auto t = tr.byId(id);
    if (t) {
      Catalog c = loadCatalog();
      std::unordered_map<int, bool> seen;
      seen[id] = true;
      auto addList = [&](const std::vector<repo::Track> &list) {
        for (auto &x : list) {
          if (items.size() >= 20) return;
          if (seen.count(x.id)) continue;
          seen[x.id] = true;
          items.push_back(makeTrack(c, x));
        }
      };
      if (t->album_id > 0) addList(tr.byAlbum(t->album_id));
      SQLite::Statement gq(db(), "SELECT genre_id FROM track_genre WHERE track_id = ?");
      gq.bind(1, id);
      while (gq.executeStep() && items.size() < 20) {
        addList(tracksByGenre(gq.getColumn(0).getInt()));
      }
    }
  }
  return respondItems(items, "", "Ascending", 0, 0);
}

// 下载原始文件（与 /Items/<id>/File 同义）。
crow::response handleDownload(const crow::request &req, const std::string &idstr) {
  int kind = 0, id = 0;
  if (!parseId(idstr, kind, id) || kind != kKindTrack) {
    return http_util::jsonResponse(404, "{}");
  }
  repo::TrackRepo tr(config::get().db_path.string());
  auto t = tr.byId(id);
  if (!t) {
    return http_util::jsonResponse(404, "{}");
  }
  return http_util::serveResource(storage::resolve(t->path),
                                  req.get_header_value("Range"));
}

// 搜索建议：曲目 / 专辑 / 艺术家。
crow::response handleSearchHints(const crow::request &req) {
  const std::string term = qs(req, "searchTerm");
  const int limit = qsInt(req, "limit", 0);
  const int cap = limit > 0 ? limit : 20;
  const std::string lterm = lower(term);
  crow::json::wvalue::list hints;
  auto push = [&](const std::string &id, const std::string &name, const char *type) {
    if ((int)hints.size() >= cap) return;
    crow::json::wvalue h;
    h["ItemId"] = id;
    h["Id"] = id;
    h["Name"] = name;
    h["Type"] = type;
    h["MediaType"] = "Audio";
    hints.emplace_back(std::move(h));
  };
  if (!term.empty()) {
    repo::TrackRepo tr(config::get().db_path.string());
    for (auto &t : tr.search(term, cap)) {
      push(makeId(kKindTrack, t.id),
           t.title.empty() ? ("Track " + std::to_string(t.id)) : t.title, "Audio");
    }
    for (auto &a : allAlbums()) {
      if (lower(a.name).find(lterm) != std::string::npos) {
        push(makeId(kKindAlbum, a.id), a.name, "MusicAlbum");
      }
    }
    for (auto &a : allArtists()) {
      if (lower(a.name).find(lterm) != std::string::npos) {
        push(makeId(kKindArtist, a.id), a.name, "MusicArtist");
      }
    }
  }
  crow::json::wvalue r;
  r["SearchHints"] = crow::json::wvalue(hints);
  r["TotalRecordCount"] = static_cast<int>(hints.size());
  return json(200, r);
}

// 过滤选项：真实流派与年份。
crow::response handleFilters(const crow::request &) {
  crow::json::wvalue::list genres;
  for (auto &g : allGenres()) genres.emplace_back(g.name);
  std::vector<int> years;
  for (auto &a : allAlbums()) {
    if (a.year > 0) years.push_back(a.year);
  }
  std::sort(years.begin(), years.end());
  years.erase(std::unique(years.begin(), years.end()), years.end());
  crow::json::wvalue::list yr;
  for (int y : years) yr.emplace_back(y);
  crow::json::wvalue r;
  r["Genres"] = crow::json::wvalue(genres);
  r["Tags"] = crow::json::wvalue(crow::json::wvalue::list{});
  r["OfficialRatings"] = crow::json::wvalue(crow::json::wvalue::list{});
  r["Years"] = crow::json::wvalue(yr);
  return json(200, r);
}

// 按名字（或 GUID）取单个艺术家。
crow::response handleArtistByName(const std::string &raw) {
  int kind = 0, id = 0;
  Catalog c = loadCatalog();
  if (parseId(raw, kind, id) && kind == kKindArtist) {
    repo::ArtistRepo ar(config::get().db_path.string());
    auto a = ar.byId(id);
    if (a) return json(200, makeArtist(c, *a).dto);
    return http_util::jsonResponse(404, "{}");
  }
  SQLite::Statement q(db(), "SELECT id, name, sort_name FROM artist WHERE name = ? LIMIT 1");
  q.bind(1, http_util::urlDecode(raw));
  if (q.executeStep()) {
    repo::Artist a;
    a.id = q.getColumn(0).getInt();
    a.name = text(q, 1);
    a.sort_name = text(q, 2);
    return json(200, makeArtist(c, a).dto);
  }
  return http_util::jsonResponse(404, "{}");
}

// 相似艺术家：与本艺术家共享流派的其它艺术家。
crow::response handleArtistSimilar(const std::string &raw) {
  int kind = 0, id = 0;
  int artistId = -1;
  if (parseId(raw, kind, id) && kind == kKindArtist) {
    artistId = id;
  } else {
    SQLite::Statement q(db(), "SELECT id FROM artist WHERE name = ? LIMIT 1");
    q.bind(1, http_util::urlDecode(raw));
    if (q.executeStep()) { artistId = q.getColumn(0).getInt(); }
  }
  if (artistId < 0) { return http_util::jsonResponse(404, "{}"); }

  std::vector<int> genreIds;
  SQLite::Statement gq(db(),
      "SELECT DISTINCT tg.genre_id FROM track t "
      "JOIN track_genre tg ON tg.track_id = t.id "
      "JOIN track_artist ta ON ta.track_id = t.id WHERE ta.artist_id = ?");
  gq.bind(1, artistId);
  while (gq.executeStep()) { genreIds.push_back(gq.getColumn(0).getInt()); }

  std::vector<int> artistIds;
  std::unordered_map<int, bool> seen;
  seen[artistId] = true;
  for (int gid : genreIds) {
    for (auto &t : tracksByGenre(gid)) {
      for (const int aid : t.artist_ids) {
        if (aid <= 0 || seen.count(aid)) { continue; }
        seen[aid] = true;
        artistIds.push_back(aid);
      }
      if (artistIds.size() >= 20) { break; }
    }
    if (artistIds.size() >= 20) { break; }
  }

  Catalog c = loadCatalog();
  repo::ArtistRepo ar(config::get().db_path.string());
  std::vector<Sortable> items;
  for (int aid : artistIds) {
    auto a = ar.byId(aid);
    if (a) { items.push_back(makeArtist(c, *a)); }
  }
  return respondItems(items, "", "Ascending", 0, 0);
}

// 解析逗号分隔的 track GUID 列表为 track id 列表。
std::vector<int> parseTrackIds(const std::string &csv) {
  std::vector<int> ids;
  for (const auto &tok : splitCsv(csv)) {
    int k = 0, v = 0;
    if (parseId(tok, k, v) && k == kKindTrack) {
      ids.push_back(v);
    }
  }
  return ids;
}

// 大小写不敏感地收集某 query 参数的全部出现（支持重复键）。
std::vector<std::string> qsList(const crow::request &req, const char *key) {
  std::vector<std::string> out;
  const std::string want = lower(std::string(key));
  for (const auto &k : req.url_params.keys()) {
    if (lower(k) != want) {
      continue;
    }
    for (char *v : req.url_params.get_list(k, false)) {
      if (v != nullptr) {
        out.emplace_back(v);
      }
    }
    for (char *v : req.url_params.get_list(k, true)) {
      if (v != nullptr) {
        out.emplace_back(v);
      }
    }
  }
  return out;
}

// 合并某 query 参数的全部出现（逗号连接）。
std::string qsJoined(const crow::request &req, const char *key) {
  std::string joined;
  for (const auto &v : qsList(req, key)) {
    if (!joined.empty()) {
      joined += ',';
    }
    joined += v;
  }
  return joined;
}

// 从 JSON body 的某个 GUID 数组字段追加 track id。
void appendBodyIds(const crow::json::rvalue &body, const char *name,
                   std::vector<int> &out) {
  if (!body || !body.has(name) ||
      body[name].t() != crow::json::type::List) {
    return;
  }
  for (const auto &x : body[name]) {
    int k = 0, v = 0;
    if (x.t() == crow::json::type::String &&
        parseId(std::string(x.s()), k, v) && k == kKindTrack) {
      out.push_back(v);
    }
  }
}

// 歌单列表。
crow::response handlePlaylists() {
  std::vector<crow::json::wvalue> items;
  for (const auto &p : playlist::list()) {
    items.push_back(makePlaylist(p).dto);
  }
  const int total = static_cast<int>(items.size());
  return json(200, queryResult(std::move(items), total, 0));
}

// 歌单详情。
crow::response handlePlaylistById(const std::string &idstr) {
  playlist::Playlist p;
  if (!playlist::get(idstr, p)) {
    return http_util::jsonResponse(404, "{}");
  }
  return json(200, playlistDto(p));
}

// 歌单内的曲目。
crow::response handlePlaylistItems(const std::string &idstr) {
  playlist::Playlist p;
  if (!playlist::get(idstr, p)) {
    return http_util::jsonResponse(404, "{}");
  }
  Catalog c = loadCatalog();
  repo::TrackRepo tr(config::get().db_path.string());
  std::vector<Sortable> items;
  for (const auto &s : p.songs) {
    if (!s.matched) {
      continue;
    }
    auto t = tr.byId(s.track_id);
    if (t) {
      Sortable item = makeTrack(c, *t);
      item.dto["PlaylistItemId"] = makeId(kKindTrack, t->id);
      item.dto["CanDelete"] = true;
      items.push_back(std::move(item));
    }
  }
  return respondItems(items, "", "Ascending", 0, 0);
}

// 新建歌单（name/ids 可来自 query 或 JSON body）。
crow::response handleCreatePlaylist(const crow::request &req) {
  const auto body = crow::json::load(req.body);
  std::string name = qs(req, "name");
  if (name.empty() && body && body.has("Name")) {
    name = body["Name"].s();
  }
  if (name.empty()) {
    return http_util::jsonResponse(400, "{}");
  }
  playlist::Playlist p = playlist::create(name);
  std::vector<int> ids = parseTrackIds(qsJoined(req, "ids"));
  appendBodyIds(body, "Ids", ids);
  std::string err;
  for (int tid : ids) {
    playlist::addTrack(p.id, tid, &err);
  }
  crow::json::wvalue r;
  r["Id"] = p.id;
  return json(200, r);
}

// 向歌单添加曲目（ids 可来自 query 或 JSON body）。
crow::response handleAddToPlaylist(const crow::request &req,
                                   const std::string &idstr) {
  playlist::Playlist p;
  if (!playlist::get(idstr, p)) {
    return http_util::jsonResponse(404, "{}");
  }
  std::vector<int> ids = parseTrackIds(qsJoined(req, "ids"));
  const auto body = crow::json::load(req.body);
  appendBodyIds(body, "Ids", ids);
  std::string err;
  for (int tid : ids) {
    playlist::addTrack(idstr, tid, &err);
  }
  return crow::response(204);
}

// 从歌单移除曲目（entryIds / ids 均按曲目 GUID 处理，可来自 query 或 body）。
crow::response handleRemoveFromPlaylist(const crow::request &req,
                                        const std::string &idstr) {
  playlist::Playlist p;
  if (!playlist::get(idstr, p)) {
    return http_util::jsonResponse(404, "{}");
  }
  std::vector<int> wanted = parseTrackIds(qsJoined(req, "entryIds"));
  if (wanted.empty()) {
    wanted = parseTrackIds(qsJoined(req, "ids"));
  }
  const auto body = crow::json::load(req.body);
  appendBodyIds(body, "EntryIds", wanted);
  appendBodyIds(body, "Ids", wanted);
  for (int tid : wanted) {
    playlist::removeTrack(idstr, tid);
  }
  return crow::response(204);
}

// 更新歌单：改名与/或替换曲目（name/ids 可来自 query 或 body）。
crow::response handleUpdatePlaylist(const crow::request &req,
                                    const std::string &idstr) {
  playlist::Playlist p;
  if (!playlist::get(idstr, p)) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto body = crow::json::load(req.body);
  std::string name = qs(req, "name");
  if (name.empty() && body && body.has("Name")) {
    name = body["Name"].s();
  }
  if (!name.empty()) {
    playlist::rename(idstr, name);
  }
  std::vector<int> ids = parseTrackIds(qsJoined(req, "ids"));
  appendBodyIds(body, "Ids", ids);
  if (!ids.empty()) {
    playlist::clear(idstr);
    std::string err;
    for (int tid : ids) {
      playlist::addTrack(idstr, tid, &err);
    }
  }
  return crow::response(204);
}

// 删除歌单。
crow::response handleDeletePlaylist(const std::string &idstr) {
  if (!playlist::remove(idstr)) {
    return http_util::jsonResponse(404, "{}");
  }
  return crow::response(204);
}

} // namespace

#define JF_ROUTE(app, url) RouteReg(&(app), (url))

void run(const config::Config &cfg) {
  setServerName(cfg.jellyfin_server_name);

  crow::SimpleApp app;

  // 系统
  JF_ROUTE(app, "/System/Info/Public")
  ([] { return json(200, publicSystemInfo()); });
  JF_ROUTE(app, "/System/Info")([] { return json(200, systemInfo()); });
  JF_ROUTE(app, "/System/Ping")
  ([] { return crow::response(200, "Jellyfin Server"); });
  JF_ROUTE(app, "/System/Ping").methods(crow::HTTPMethod::Post)
  ([](const crow::request &) { return crow::response(200, "Jellyfin Server"); });
  JF_ROUTE(app, "/System/Endpoint")([] {
    crow::json::wvalue r;
    r["IsLocal"] = true;
    r["IsInNetwork"] = true;
    return json(200, r);
  });

  // 用户/鉴权
  JF_ROUTE(app, "/Users/Public")([] {
    std::vector<crow::json::wvalue> items;
    items.push_back(userDto());
    crow::json::wvalue arr;
    arr = std::move(items);
    return json(200, arr);
  });
  JF_ROUTE(app, "/Users/AuthenticateByName").methods(crow::HTTPMethod::Post)
  ([](const crow::request &req) {
    std::string client;
    auto body = crow::json::load(req.body);
    if (body && body.has("Username")) {
      client = body["Username"].s();
    }
    crow::json::wvalue r;
    r["User"] = userDto();
    r["SessionInfo"] = sessionInfo(client, "");
    r["AccessToken"] = "musicd-token";
    r["ServerId"] = serverId();
    return json(200, r);
  });
  JF_ROUTE(app, "/Users/Me")([] { return json(200, userDto()); });
  JF_ROUTE(app, "/Users/<string>")
  ([](const crow::request &, std::string) { return json(200, userDto()); });

  // 视图
  JF_ROUTE(app, "/UserViews")([] { return userViews(); });
  JF_ROUTE(app, "/Users/<string>/Views")
  ([](const crow::request &, std::string) { return userViews(); });

  // 条目
  JF_ROUTE(app, "/Items/Latest")
  ([](const crow::request &req) { return handleLatest(req); });
  JF_ROUTE(app, "/Items/Filters")
  ([](const crow::request &req) { return handleFilters(req); });
  JF_ROUTE(app, "/Users/<string>/Items/Latest")
  ([](const crow::request &req, std::string) { return handleLatest(req); });
  JF_ROUTE(app, "/Items")
  ([](const crow::request &req) { return handleItems(req); });
  JF_ROUTE(app, "/Users/<string>/Items")
  ([](const crow::request &req, std::string) { return handleItems(req); });
  JF_ROUTE(app, "/Users/<string>/Items/<string>")
  ([](const crow::request &, std::string, std::string itemId) {
    return handleItem(itemId);
  });
  JF_ROUTE(app, "/Items/<string>/PlaybackInfo")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post)
  ([](const crow::request &req, std::string itemId) { return handlePlaybackInfo(req, itemId); });
  JF_ROUTE(app, "/Items/<string>/Images/<string>/<string>")
  ([](const crow::request &, std::string itemId, std::string imageType, std::string imageIndex) {
    return handleImageType(itemId, imageType, imageIndex);
  });
  JF_ROUTE(app, "/Items/<string>/Images/<string>")
  ([](const crow::request &, std::string itemId, std::string imageType) {
    return handleImageType(itemId, imageType, "");
  });
  JF_ROUTE(app, "/Items/<string>/Download")
  ([](const crow::request &req, std::string itemId) { return handleDownload(req, itemId); });
  JF_ROUTE(app, "/Items/<string>/File")
  ([](const crow::request &req, std::string itemId) { return handleDownload(req, itemId); });
  JF_ROUTE(app, "/Items/<string>/Similar")
  ([](const crow::request &, std::string itemId) { return handleSimilar(itemId); });
  JF_ROUTE(app, "/Items/<string>")
  ([](const crow::request &, std::string itemId) { return handleItem(itemId); });
  // 通用删除项（官方 DeleteItem）：目前仅支持删除歌单，其他类型返回 404。
  JF_ROUTE(app, "/Items/<string>").methods(crow::HTTPMethod::Delete)
  ([](const crow::request &, std::string itemId) {
    return handleDeletePlaylist(itemId);
  });

  // 流媒体
  JF_ROUTE(app, "/Audio/<string>/Lyrics")
  ([](const crow::request &, std::string itemId) { return handleLyrics(itemId); });
  JF_ROUTE(app, "/Audio/<string>/<string>")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Head)
  ([](const crow::request &req, std::string itemId, std::string) {
    return handleStream(req, itemId);
  });

  // 艺术家/流派
  JF_ROUTE(app, "/Artists")
  ([](const crow::request &req) { return handleArtists(req); });
  JF_ROUTE(app, "/Artists/AlbumArtists")
  ([](const crow::request &req) { return handleArtists(req, true); });
  JF_ROUTE(app, "/Artists/<string>/Images/<string>")
  ([](const crow::request &, std::string name, std::string imageType) {
    return handleImageType(name, imageType, "");
  });
  JF_ROUTE(app, "/Artists/<string>/Similar")
  ([](const crow::request &, std::string name) { return handleArtistSimilar(name); });
  JF_ROUTE(app, "/Artists/<string>")
  ([](const crow::request &, std::string name) {
    return handleArtistByName(name);
  });
  JF_ROUTE(app, "/MusicGenres")
  ([](const crow::request &req) { return handleGenres(req); });
  JF_ROUTE(app, "/MusicGenres/<string>")
  ([](const crow::request &, std::string name) {
    return handleMusicGenreByName(http_util::urlDecode(name));
  });
  JF_ROUTE(app, "/Genres")
  ([](const crow::request &req) { return handleGenres(req); });
  JF_ROUTE(app, "/Genres/<string>")
  ([](const crow::request &, std::string name) {
    return handleMusicGenreByName(http_util::urlDecode(name));
  });
  JF_ROUTE(app, "/Studios")
  ([](const crow::request &) {
    std::vector<crow::json::wvalue> empty;
    return json(200, queryResult(std::move(empty), 0, 0));
  });
  JF_ROUTE(app, "/Studios/<string>")
  ([](const crow::request &, std::string) {
    std::vector<crow::json::wvalue> empty;
    return json(200, queryResult(std::move(empty), 0, 0));
  });

  // 播放会话
  JF_ROUTE(app, "/Sessions")([] { return emptyList(); });
  JF_ROUTE(app, "/Sessions/Playing").methods(crow::HTTPMethod::Post)
  ([](const crow::request &) { return crow::response(204); });
  JF_ROUTE(app, "/Sessions/Playing/Progress").methods(crow::HTTPMethod::Post)
  ([](const crow::request &) { return crow::response(204); });
  JF_ROUTE(app, "/Sessions/Playing/Stopped").methods(crow::HTTPMethod::Post)
  ([](const crow::request &) { return crow::response(204); });

  // 收藏
  JF_ROUTE(app, "/UserFavoriteItems/<string>").methods(crow::HTTPMethod::Post)
  ([](const crow::request &, std::string) {
    crow::json::wvalue r;
    r["IsFavorite"] = true;
    r["PlayCount"] = 0;
    r["Played"] = false;
    r["Key"] = "";
    return json(200, r);
  });
  JF_ROUTE(app, "/UserFavoriteItems/<string>")
      .methods(crow::HTTPMethod::Delete)
  ([](const crow::request &, std::string) {
    crow::json::wvalue r;
    r["IsFavorite"] = false;
    r["PlayCount"] = 0;
    r["Played"] = false;
    r["Key"] = "";
    return json(200, r);
  });

  // 占位/默认值
  JF_ROUTE(app, "/Branding/Configuration")
  ([] { return json(200, brandingOptions()); });
  JF_ROUTE(app, "/DisplayPreferences/<string>")
  ([](const crow::request &, std::string id) {
    return json(200, displayPreferences(id));
  });
  JF_ROUTE(app, "/DisplayPreferences/<string>").methods(crow::HTTPMethod::Post)
  ([](const crow::request &, std::string) { return crow::response(204); });
  JF_ROUTE(app, "/ScheduledTasks")([] { return emptyList(); });
  JF_ROUTE(app, "/Plugins")([] { return emptyList(); });
  JF_ROUTE(app, "/Search/Hints")
  ([](const crow::request &req) { return handleSearchHints(req); });
  JF_ROUTE(app, "/Shows/NextUp")([] {
    std::vector<crow::json::wvalue> empty;
    return json(200, queryResult(std::move(empty), 0, 0));
  });
  JF_ROUTE(app, "/UserItems/Resume")([] {
    std::vector<crow::json::wvalue> empty;
    return json(200, queryResult(std::move(empty), 0, 0));
  });
  JF_ROUTE(app, "/Playlists")([] { return handlePlaylists(); });
  JF_ROUTE(app, "/Playlists").methods(crow::HTTPMethod::Post)
  ([](const crow::request &req) { return handleCreatePlaylist(req); });
  JF_ROUTE(app, "/Playlists/<string>")
  ([](const crow::request &, std::string id) { return handlePlaylistById(id); });
  JF_ROUTE(app, "/Playlists/<string>").methods(crow::HTTPMethod::Post)
  ([](const crow::request &req, std::string id) {
    return handleUpdatePlaylist(req, id);
  });
  JF_ROUTE(app, "/Playlists/<string>").methods(crow::HTTPMethod::Delete)
  ([](const crow::request &, std::string id) { return handleDeletePlaylist(id); });
  JF_ROUTE(app, "/Playlists/<string>/Items")
  ([](const crow::request &, std::string id) { return handlePlaylistItems(id); });
  JF_ROUTE(app, "/Playlists/<string>/Items").methods(crow::HTTPMethod::Post)
  ([](const crow::request &req, std::string id) {
    return handleAddToPlaylist(req, id);
  });
  JF_ROUTE(app, "/Playlists/<string>/Items").methods(crow::HTTPMethod::Delete)
  ([](const crow::request &req, std::string id) {
    return handleRemoveFromPlaylist(req, id);
  });
  JF_ROUTE(app, "/Localization/Options")([] {
    crow::json::wvalue arr;
    arr = crow::json::wvalue::list{};
    return json(200, arr);
  });
  JF_ROUTE(app, "/Users/<string>/GroupingOptions")
  ([](const crow::request &, std::string) {
    crow::json::wvalue r;
    r["GroupingOptions"] = crow::json::wvalue::list{};
    return json(200, r);
  });
  JF_ROUTE(app, "/QuickConnect/Enabled")([] {
    crow::json::wvalue r;
    r = false;
    return json(200, r);
  });
  JF_ROUTE(app, "/System/Configuration")([] {
    crow::json::wvalue r;
    return json(200, r);
  });
  JF_ROUTE(app, "/Users/<string>/Configuration")
  ([](const crow::request &, std::string) {
    crow::json::wvalue r;
    return json(200, r);
  });

  // 路由大小写不敏感：Crow 的字面量路由默认大小写敏感，这里开启全局开关，
  // 使路由匹配忽略大小写（参数值保持原样）。Jellyfin 客户端会以各种大小写请求。
  crow::case_insensitive_routing() = true;

  app.loglevel(crow::LogLevel::Info);
  app.port(cfg.jellyfin_port).multithreaded().concurrency(32).run();
}

} // namespace jellyfin
