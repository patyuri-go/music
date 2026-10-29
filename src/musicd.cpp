#include "config.h"
#include "crow/app.h"
#include "crow/http_request.h"
#include "crow/http_response.h"
#include "crow/json.h"

#include "crow/logging.h"
#include "database.h"
#include "db.h"
#include "http_util.h"
#include "image_cache.h"
#include "jellyfin.h"
#include "media_library/library_runtime.h"
#include "metadata.h"
#include "api/webdav/webdav.h"
#include "schema.h"
#include "scanner.h"
#include "storage.h"
#include "api/playlist/playlist.h"
#include "service.h"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

using http_util::jsonResponse;
using http_util::serveFile;
using http_util::urlDecode;

namespace {

std::string rootTypeOf(const std::string &path) {
  return webdav::isWebdavUrl(path) ? "webdav" : "local";
}

std::string deriveRootName(const std::string &path) {
  std::string p = path;
  while (!p.empty() && p.back() == '/') {
    p.pop_back();
  }
  const std::size_t slash = p.find_last_of('/');
  std::string last = (slash == std::string::npos) ? p : p.substr(slash + 1);
  if (last.empty()) {
    last = p;
  }
  return urlDecode(last);
}

void bootstrap() {
  const auto &cfg = config::load();
  auto &database = db::conn(cfg.db_path.string());
  schema::migrate(database);
  schema::selfCheck(database);

  std::cout << "[boot] db=" << cfg.db_path
            << " static_dir=" << cfg.static_dir
            << " page_size=" << cfg.page_size << std::endl;
  std::cout << "[boot] schema 版本 " << schema::kVersion << "，启动自检通过"
            << std::endl;

  {
    repo::ScanRootRepo roots(cfg.db_path.string());
    const auto existing = roots.list();
    const auto ensureRoot = [&existing, &roots](const std::string &path) {
      if (path.empty()) {
        return;
      }
      for (const auto &r : existing) {
        if (r.path == path) {
          return;
        }
      }
      roots.add(path, "", "", deriveRootName(path), rootTypeOf(path));
    };
    for (const auto &root : cfg.scan_roots) {
      ensureRoot(root.string());
    }
    for (const auto &w : cfg.webdav_roots) {
      ensureRoot(w.url);
    }
    for (const auto &r : roots.list()) {
      if (r.name.empty()) {
        roots.updateName(r.id, deriveRootName(r.path));
      }
    }
  }

  if (cfg.scan_on_start) {
    scanner::enqueueJob("scan-all", [] { scanner::scanAllBlocking(); });
  }

  playlist::loadAll();
}

std::optional<fs::path> resolveWithin(const fs::path &root,
                                      const std::string &relative) {
  std::error_code ec;
  const fs::path base = fs::weakly_canonical(root, ec);
  if (ec) {
    return std::nullopt;
  }
  const fs::path target = fs::weakly_canonical(root / relative, ec);
  if (ec) {
    return std::nullopt;
  }

  const fs::path rel = target.lexically_relative(base);
  if (rel.empty()) {
    return std::nullopt;
  }
  for (const auto &part : rel) {
    if (part == "..") {
      return std::nullopt;
    }
  }
  return target;
}

bool isUnderAnyRoot(const fs::path &target) {
  std::error_code ec;
  const fs::path canonical = fs::weakly_canonical(target, ec);
  if (ec) {
    return false;
  }
  repo::ScanRootRepo repo(config::get().db_path.string());
  for (const auto &root : repo.list()) {
    fs::path base = fs::weakly_canonical(root.path, ec);
    if (ec) {
      continue;
    }
    const fs::path rel = canonical.lexically_relative(base);
    if (!rel.empty() && rel.native().rfind("..", 0) != 0) {
      for (const auto &part : rel) {
        if (part == "..") {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

crow::response browseWebdavRoot(int root_id, const std::string &root_url,
                                const std::string &rel_path) {
  try {
    webdav::Url base = webdav::resolveCredentials(root_url);
    if (base.path.empty()) {
      base.path = "/";
    }
    std::string dir = base.path;
    while (dir.size() > 1 && dir.back() == '/') {
      dir.pop_back();
    }
    {
      std::size_t start = 0;
      while (start <= rel_path.size()) {
        const std::size_t slash = rel_path.find('/', start);
        const std::string seg = (slash == std::string::npos)
                                    ? rel_path.substr(start)
                                    : rel_path.substr(start, slash - start);
        if (!seg.empty()) {
          dir = webdav::joinPath(dir, seg);
        }
        if (slash == std::string::npos) {
          break;
        }
        start = slash + 1;
      }
    }

    const std::string req_path = (dir == "/") ? "/" : dir + "/";
    webdav::Client client(base);
    webdav::Response res = client.propfind(req_path, 1);
    if (res.status != 207) {
      return jsonResponse(400, "{\"erro\":true}");
    }
    std::vector<webdav::Resource> items = webdav::parsePropfind(res.body);

    // 服务器的 href 可能与我们配置的 URL 百分号编码方式不同（如 [ -> %5B、
    // 空格 -> %20、非 ASCII -> UTF-8 转义），因此比较时统一解码，
    // 并用服务器返回的“自身”条目来确定当前目录的编码形式，保证 DB 前缀匹配。
    const std::string dir_dec = urlDecode(dir);
    std::string server_dir;
    for (const auto &item : items) {
      std::string ipath = item.path;
      while (ipath.size() > 1 && ipath.back() == '/') {
        ipath.pop_back();
      }
      if (urlDecode(ipath) == dir_dec) {
        server_dir = ipath;
        break;
      }
    }
    const std::string dir_encoded = server_dir.empty() ? dir : server_dir;
    const std::string dir_url = webdav::buildUrl(base, dir_encoded);

    repo::ScanRootRepo root_repo(config::get().db_path.string());
    auto root_opt = root_repo.byId(root_id);
    const std::string vbase =
        root_opt ? (root_opt->name + (rel_path.empty() ? "" : "/" + rel_path))
                 : dir_url;
    const std::string vprefix = vbase + "/";

    struct Entry {
      std::string name;
      std::string path;
      bool isDir = false;
      bool onDisk = false;
      bool inDb = false;
      int trackId = 0;
      std::string title;
    };
    std::map<std::string, Entry> merged;

    {
      SQLite::Database &db = db::conn(config::get().db_path.string());
      SQLite::Statement q(db, schema::sql::kTrackPathsUnderRoot);
      q.bind(1, root_id);
      q.bind(2, vprefix);
      q.bind(3, vbase + "0");
      while (q.executeStep()) {
        const std::string vp = q.getColumn(1).getText();
        if (vp.compare(0, vprefix.size(), vprefix) != 0) {
          continue;
        }
        const std::string rest = vp.substr(vprefix.size());
        if (rest.empty() || rest.find('/') != std::string::npos) {
          continue;
        }
        const std::string real = root_opt ? storage::toReal(*root_opt, vp) : vp;
        Entry &e = merged[urlDecode(real)];
        e.path = real;
        e.inDb = true;
        e.trackId = q.getColumn(0).getInt();
        e.title = q.getColumn(2).getText();
        const std::size_t slash = real.rfind('/');
        e.name = urlDecode((slash == std::string::npos) ? real
                                                        : real.substr(slash + 1));
      }
    }

    for (const auto &item : items) {
      std::string ipath = item.path;
      if (ipath.size() > 1 && ipath.back() == '/') {
        ipath.pop_back();
      }
      if (urlDecode(ipath) == dir_dec) {
        continue;
      }
      const std::size_t slash = ipath.rfind('/');
      const std::string raw_name =
          (slash == std::string::npos) ? ipath : ipath.substr(slash + 1);
      if (raw_name.empty()) {
        continue;
      }
      const std::string full = webdav::buildUrl(base, ipath);
      Entry &e = merged[urlDecode(full)];
      e.path = full;
      e.onDisk = true;
      e.isDir = item.collection;
      e.name = urlDecode(raw_name);
    }

    crow::json::wvalue out(crow::json::type::List);
    int i = 0;
    for (const auto &kv : merged) {
      const Entry &e = kv.second;
      out[i]["name"] = e.name;
      out[i]["isDir"] = e.isDir;
      out[i]["path"] = e.path;
      out[i]["inDb"] = e.inDb;
      out[i]["onDisk"] = e.onDisk;
      out[i]["trackId"] = e.trackId;
      out[i]["title"] = e.title;
      out[i]["status"] =
          (e.inDb && e.onDisk) ? "both" : (e.inDb ? "db" : "disk");
      ++i;
    }
    return jsonResponse(200, out.dump());
  } catch (const std::exception &e) {
    return jsonResponse(500, "{\"erro\":true}");
  }
}

crow::response browseDir(int root_id, const std::string &rel_path) {
  try {
    if (root_id <= 0) {
      return jsonResponse(400, "{\"erro\":true}");
    }

    repo::ScanRootRepo root_repo(config::get().db_path.string());
    auto root_opt = root_repo.byId(root_id);
    if (!root_opt) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    if (webdav::isWebdavUrl(root_opt->path)) {
      return browseWebdavRoot(root_id, root_opt->path, rel_path);
    }
    fs::path base = fs::weakly_canonical(root_opt->path);
    fs::path target = base / rel_path;

    std::error_code ec;
    if (!fs::is_directory(target, ec)) {
      return jsonResponse(400, "{\"erro\":true}");
    }

    const std::string vbase =
        root_opt->name + (rel_path.empty() ? "" : "/" + rel_path);
    const std::string vprefix = vbase + "/";

    struct Entry {
      std::string name;
      std::string path;
      bool isDir = false;
      bool onDisk = false;
      bool inDb = false;
      int trackId = 0;
      std::string title;
    };
    std::map<std::string, Entry> merged;

    {
      SQLite::Database &db = db::conn(config::get().db_path.string());
      SQLite::Statement q(db, schema::sql::kTrackPathsUnderRoot);
      q.bind(1, root_id);
      q.bind(2, vprefix);
      q.bind(3, vbase + "0");
      while (q.executeStep()) {
        const std::string vp = q.getColumn(1).getText();
        if (vp.compare(0, vprefix.size(), vprefix) != 0) {
          continue;
        }
        const std::string rest = vp.substr(vprefix.size());
        if (rest.empty() || rest.find('/') != std::string::npos) {
          continue;
        }
        const std::string real = storage::toReal(*root_opt, vp);
        Entry &e = merged[real];
        e.path = real;
        e.inDb = true;
        e.trackId = q.getColumn(0).getInt();
        e.title = q.getColumn(2).getText();
        e.name = fs::path(real).filename().string();
      }
    }

    std::error_code ec_dir;
    for (const auto &dir_entry : fs::directory_iterator(target, ec_dir)) {
      if (ec_dir) break;
      const fs::path &p = dir_entry.path();
      std::error_code ec_type;
      bool is_dir = false;
      try {
        is_dir = fs::is_directory(p, ec_type);
      } catch (...) {
        continue;
      }
      if (ec_type) continue;

      std::string path_str;
      std::error_code ec_can;
      try {
        path_str = fs::canonical(p, ec_can).string();
      } catch (...) {
        path_str = p.string();
      }
      if (ec_can) {
        path_str = p.string();
      }

      Entry &e = merged[path_str];
      e.path = path_str;
      e.onDisk = true;
      e.isDir = is_dir;
      e.name = p.filename().string();
    }

    crow::json::wvalue out(crow::json::type::List);
    int i = 0;
    for (const auto &kv : merged) {
      const Entry &e = kv.second;
      out[i]["name"] = e.name;
      out[i]["isDir"] = e.isDir;
      out[i]["path"] = e.path;
      out[i]["inDb"] = e.inDb;
      out[i]["onDisk"] = e.onDisk;
      out[i]["trackId"] = e.trackId;
      out[i]["title"] = e.title;
      out[i]["status"] = (e.inDb && e.onDisk)
                             ? "both"
                             : (e.inDb ? "db" : "disk");
      ++i;
    }
    return jsonResponse(200, out.dump());
  } catch (const std::exception &e) {
    return jsonResponse(500, "{\"erro\":true}");
  }
}

std::string media_library_lower(const std::string &text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

std::string artistKeyOf(const std::vector<std::string> &names) {
  std::vector<std::string> sorted;
  for (const auto &n : names) {
    std::string lowered;
    for (const char c : n) {
      lowered.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (!lowered.empty()) {
      sorted.push_back(lowered);
    }
  }
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  std::string key;
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    if (i != 0) {
      key += '|';
    }
    key += sorted[i];
  }
  return key;
}

int ensureArtistId(SQLite::Database &db, const std::string &name) {
  if (name.empty()) {
    return 0;
  }
  SQLite::Statement insert(db, schema::sql::kArtistInsert);
  insert.bind(1, name);
  insert.executeStep();
  SQLite::Statement query(db, schema::sql::kArtistByName);
  query.bind(1, name);
  return query.executeStep() ? query.getColumn(0).getInt() : 0;
}

std::vector<int> ensureArtistIds(SQLite::Database &db,
                                 const std::vector<std::string> &names) {
  std::vector<int> ids;
  for (const auto &n : names) {
    const int id = ensureArtistId(db, n);
    if (id != 0) {
      ids.push_back(id);
    }
  }
  return ids;
}

int ensureAlbumId(SQLite::Database &db, const std::string &name,
                  const std::string &key) {
  SQLite::Statement insert(db, schema::sql::kAlbumUpsert);
  insert.bind(1, name);
  insert.bind(2, key);
  insert.executeStep();
  SQLite::Statement query(db, schema::sql::kAlbumByKey);
  query.bind(1, name);
  query.bind(2, key);
  return query.executeStep() ? query.getColumn(0).getInt() : 0;
}

void setTrackArtists(SQLite::Database &db, int track_id,
                     const std::vector<int> &artist_ids) {
  SQLite::Statement clear(db, schema::sql::kTrackArtistClear);
  clear.bind(1, track_id);
  clear.executeStep();
  int position = 0;
  for (const int aid : artist_ids) {
    SQLite::Statement insert(db, schema::sql::kTrackArtistInsert);
    insert.bind(1, track_id);
    insert.bind(2, aid);
    insert.bind(3, position++);
    insert.executeStep();
  }
}

void setAlbumArtists(SQLite::Database &db, int album_id,
                     const std::vector<int> &artist_ids) {
  SQLite::Statement clear(db, schema::sql::kAlbumArtistClear);
  clear.bind(1, album_id);
  clear.executeStep();
  int position = 0;
  for (const int aid : artist_ids) {
    SQLite::Statement insert(db, schema::sql::kAlbumArtistInsert);
    insert.bind(1, album_id);
    insert.bind(2, aid);
    insert.bind(3, position++);
    insert.executeStep();
  }
}

bool syncFromFileById(int id, int slot = -1) {
  repo::TrackRepo track_repo(config::get().db_path.string());
  auto track = track_repo.byId(id);
  if (!track) {
    return false;
  }
  const std::string real_path = storage::resolve(track->path);
  if (!webdav::isWebdavUrl(real_path) && !fs::exists(real_path)) {
    return false;
  }
  if (slot >= 0) {
    scanner::activePathsSet(slot, real_path);
  } else {
    scanner::setCurrentFile(real_path);
  }
  auto meta = metadata::readFileMeta(real_path);
  if (!meta) {
    return false;
  }
  SQLite::Database &db = db::conn(config::get().db_path.string());

  std::vector<std::string> artist_names = meta->artists;
  if (artist_names.empty() && !meta->artist.empty()) {
    artist_names.push_back(meta->artist);
  }
  const std::vector<int> artist_ids = ensureArtistIds(db, artist_names);

  std::vector<std::string> album_artist_names = meta->album_artists;
  if (album_artist_names.empty()) {
    album_artist_names = artist_names;
  }
  const int album_id =
      ensureAlbumId(db, meta->album, artistKeyOf(album_artist_names));

  repo::Track t = *track;
  t.title = meta->title;
  t.album_id = album_id;
  t.track_no = meta->track_no;
  t.disc_no = meta->disc_no;
  track_repo.updatePartial(t);

  setTrackArtists(db, id, artist_ids);
  if (album_id != 0) {
    setAlbumArtists(db, album_id, ensureArtistIds(db, album_artist_names));
  }

  SQLite::Statement del(db, "DELETE FROM track_genre WHERE track_id = ?");
  del.bind(1, id);
  del.executeStep();
  if (!meta->genre.empty()) {
    SQLite::Statement genre_q(db, schema::sql::kGenreByName);
    genre_q.bind(1, meta->genre);
    int genre_id = 0;
    if (genre_q.executeStep()) {
      genre_id = genre_q.getColumn(0).getInt();
    }
    if (genre_id == 0) {
      SQLite::Statement genre_ins(db, schema::sql::kGenreInsert);
      genre_ins.bind(1, meta->genre);
      genre_ins.executeStep();
      SQLite::Statement genre_q2(db, schema::sql::kGenreByName);
      genre_q2.bind(1, meta->genre);
      if (genre_q2.executeStep()) {
        genre_id = genre_q2.getColumn(0).getInt();
      }
    }
    if (genre_id > 0) {
      SQLite::Statement tg_ins(db, schema::sql::kTrackGenreInsert);
      tg_ins.bind(1, id);
      tg_ins.bind(2, genre_id);
      tg_ins.executeStep();
    }
  }
  // The track may have moved to another album / artists; drop now-empty rows.
  db.exec("DELETE FROM album WHERE id NOT IN "
          "(SELECT DISTINCT album_id FROM track WHERE album_id IS NOT NULL)");
  db.exec("DELETE FROM artist WHERE id NOT IN "
          "(SELECT DISTINCT artist_id FROM track_artist) AND id NOT IN "
          "(SELECT DISTINCT artist_id FROM album_artist)");
  // Import lyrics from the file: embedded lyrics preferred, else sibling .lrc.
  {
    std::string lyrics = meta->lyrics;
    if (lyrics.empty()) {
      lyrics = metadata::readSiblingLyrics(real_path);
    }
    if (!lyrics.empty()) {
      SQLite::Statement lyr(db, schema::sql::kTrackLyricsUpsert);
      lyr.bind(1, id);
      lyr.bind(2, lyrics);
      lyr.bind(3, metadata::isSyncedLyrics(lyrics) ? 1 : 0);
      lyr.executeStep();
    }
  }
  return true;
}

bool syncToFileById(int id, int slot = -1) {
  repo::TrackRepo track_repo(config::get().db_path.string());
  auto track = track_repo.byId(id);
  if (!track) {
    return false;
  }
  const std::string real_path = storage::resolve(track->path);
  if (!webdav::isWebdavUrl(real_path) && !fs::exists(real_path)) {
    return false;
  }
  if (slot >= 0) {
    scanner::activePathsSet(slot, real_path);
  } else {
    scanner::setCurrentFile(real_path);
  }
  SQLite::Database &db = db::conn(config::get().db_path.string());
  repo::ArtistRepo artist_repo(config::get().db_path.string());
  repo::AlbumRepo album_repo(config::get().db_path.string());

  metadata::FileMeta meta;
  meta.title = track->title;
  meta.track_no = track->track_no;
  meta.disc_no = track->disc_no;
  for (const int aid : track->artist_ids) {
    if (auto artist = artist_repo.byId(aid)) {
      meta.artists.push_back(artist->name);
    }
  }
  if (!meta.artists.empty()) {
    meta.artist = meta.artists.front();
  }
  if (auto album = album_repo.byId(track->album_id)) {
    meta.album = album->name;
    meta.year = album->year;
    for (const int aid : album->artist_ids) {
      if (auto artist = artist_repo.byId(aid)) {
        meta.album_artists.push_back(artist->name);
      }
    }
  }
  meta.genre = "";
  meta.comment = "";

  auto genres = track_repo.genresByTrackId(id);
  if (!genres.empty()) {
    SQLite::Statement q(db, "SELECT name FROM genre WHERE id = ?");
    q.bind(1, genres[0]);
    if (q.executeStep()) {
      meta.genre = q.getColumn(0).getText();
    }
  }
  return metadata::writeFileMeta(real_path, meta);
}

// Run fn over every id using up to config::scan_threads worker threads. The
// per-thread SQLite connections (db::conn is thread_local) plus WAL/busy_timeout
// make concurrent sync operations safe.
void runIdsParallel(const std::vector<int> &ids,
                    const std::function<void(int, int)> &fn) {
  if (ids.empty()) {
    return;
  }
  int threads = config::get().scan_threads;
  if (threads < 1) {
    threads = 1;
  }
  if (threads > static_cast<int>(ids.size())) {
    threads = static_cast<int>(ids.size());
  }
  scanner::activePathsBegin(threads);
  std::atomic<std::size_t> next{0};
  std::vector<std::thread> pool;
  pool.reserve(static_cast<std::size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&ids, &fn, &next, t]() {
      while (true) {
        const std::size_t i = next.fetch_add(1);
        if (i >= ids.size()) {
          break;
        }
        fn(ids[i], t);
      }
      scanner::activePathsSet(t, std::string());
    });
  }
  for (auto &th : pool) {
    th.join();
  }
  scanner::activePathsEnd();
}

// Expand a file or directory (real path / WebDAV URL) into all track ids that
// live under it. Used by batch sync-from-file / sync-to-file on folders.
std::vector<int> tracksUnderPath(int root_id, const std::string &path) {
  std::vector<int> ids;
  repo::ScanRootRepo root_repo(config::get().db_path.string());
  auto root = root_repo.byId(root_id);
  if (!root) {
    return ids;
  }
  const std::string vpath = storage::toVirtual(*root, path);
  SQLite::Database &db = db::conn(config::get().db_path.string());
  SQLite::Statement q(db,
                      "SELECT id FROM track WHERE root_id = ? AND "
                      "(path = ? OR (path >= ? AND path < ?)) ORDER BY id");
  q.bind(1, root_id);
  q.bind(2, vpath);
  q.bind(3, vpath + "/");
  q.bind(4, vpath + "0");
  while (q.executeStep()) {
    ids.push_back(q.getColumn(0).getInt());
  }
  return ids;
}

std::vector<int> tracksUnderAlbum(int album_id) {
  std::vector<int> ids;
  SQLite::Database &db = db::conn(config::get().db_path.string());
  SQLite::Statement q(db, "SELECT id FROM track WHERE album_id = ? ORDER BY id");
  q.bind(1, album_id);
  while (q.executeStep()) {
    ids.push_back(q.getColumn(0).getInt());
  }
  return ids;
}

std::vector<int> tracksUnderArtist(int artist_id) {
  std::vector<int> ids;
  SQLite::Database &db = db::conn(config::get().db_path.string());
  SQLite::Statement q(
      db,
      "SELECT DISTINCT t.id FROM track t "
      "JOIN album_artist aa ON aa.album_id = t.album_id "
      "WHERE aa.artist_id = ? ORDER BY t.id");
  q.bind(1, artist_id);
  while (q.executeStep()) {
    ids.push_back(q.getColumn(0).getInt());
  }
  return ids;
}

crow::json::wvalue playlistSummaryJson(const playlist::Playlist &p) {
  crow::json::wvalue w;
  w["id"] = p.id;
  w["name"] = p.name;
  w["song_count"] = playlist::songCount(p);
  w["total_duration_ms"] = playlist::totalDurationMs(p);
  return w;
}

crow::json::wvalue playlistDetailJson(const playlist::Playlist &p) {
  crow::json::wvalue w = playlistSummaryJson(p);
  crow::json::wvalue::list songs;
  for (const auto &s : p.songs) {
    crow::json::wvalue sw;
    sw["path"] = s.path;
    sw["album"] = s.album;
    sw["album_artist"] = s.album_artist;
    sw["matched"] = s.matched;
    sw["track_id"] = s.track_id;
    sw["title"] = s.title;
    sw["artist"] = s.artist;
    songs.emplace_back(std::move(sw));
  }
  w["songs"] = crow::json::wvalue(songs);
  return w;
}

} // namespace

int main(int argc, char *argv[]) {
  try {
    bootstrap();
  } catch (const std::exception &e) {
    std::cerr << "[fatal] 启动失败: " << e.what() << std::endl;
    return 1;
  }

  try {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      auto next = [&](const char *name) -> std::string {
        if (i + 1 >= argc) {
          throw std::runtime_error(std::string(name) + " 缺少参数值");
        }
        return argv[++i];
      };
      if (arg == "--port") {
        config::setPort(std::stoi(next("--port")));
      } else if (arg == "--jellyfin-port") {
        config::setJellyfinPort(std::stoi(next("--jellyfin-port")));
      } else if (arg == "--jellyfin-enabled") {
        const std::string v = next("--jellyfin-enabled");
        config::setJellyfinEnabled(v == "1" || v == "true" || v == "TRUE");
      } else if (arg == "--help" || arg == "-h") {
        std::cout << "用法: musicd [选项]\n"
                  << "  --port N               主服务端口 (默认 5666)\n"
                  << "  --jellyfin-port N      Jellyfin 端口 (默认 8096)\n"
                  << "  --jellyfin-enabled B   是否启用 Jellyfin (1/0)\n"
                  << "  -h, --help             显示帮助\n";
        return 0;
      } else {
        std::cerr << "[fatal] 未知参数: " << arg << "（用 --help 查看用法）"
                  << std::endl;
        return 1;
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "[fatal] 参数错误: " << e.what() << std::endl;
    return 1;
  }

  const auto &cfg = config::get();

  image_cache::Options cache_options;
  cache_options.enabled = cfg.image_cache.enabled;
  cache_options.dir = cfg.image_cache.dir;
  cache_options.max_bytes = cfg.image_cache.max_bytes;
  cache_options.max_file_bytes = cfg.image_cache.max_file_bytes;
  cache_options.ttl_days = cfg.image_cache.ttl_days;
  cache_options.gc_interval_seconds = cfg.image_cache.gc_interval_seconds;
  cache_options.revalidate_seconds = cfg.image_cache.revalidate_seconds;
  std::string cache_error;
  if (!image_cache::init(cache_options, cache_error)) {
    std::cerr << "[image_cache] disabled: " << cache_error << std::endl;
  }

  // 主服务与 Jellyfin 兼容层共用同一个 Library 对象。
  try {
    media_library::initialize_library("local-main", cfg.jellyfin_server_name,
                                      cfg.db_path.string());
  } catch (const std::exception &e) {
    std::cerr << "[fatal] 媒体库初始化失败: " << e.what() << std::endl;
    return 1;
  }

  crow::SimpleApp app;
  app.loglevel(crow::LogLevel::WARNING);

  CROW_ROUTE(app, "/")([&cfg]() {
    crow::response res;
    res.set_static_file_info((cfg.static_dir / "index.html").string());
    return res;
  });

  CROW_ROUTE(app, "/web/<path>")([&cfg](std::string path) {
    crow::response res;
    auto target = resolveWithin(cfg.static_dir, path);
    if (!target) {
      res.code = 403;
      res.body = "forbidden";
      return res;
    }
    res.set_static_file_info_unsafe(target->string());
    return res;
  });

  CROW_ROUTE(app, "/api/query")
      .methods("POST"_method)([](const crow::request &req) {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("type")) {
          return jsonResponse(400, "{\"erro\":true}");
        }
        Service service;
        auto result = service.query(json);
        if (!result) {
          return jsonResponse(404, "{}");
        }
        return jsonResponse(200, result->dump());
      });

  CROW_ROUTE(app, "/api/track/<int>")([](int id) {
    Service service;
    auto result = service.trackById(id);
    if (!result) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    auto &library = media_library::library();
    const auto ref =
        library.ref_for_compat(media_library::EntityType::Track, id);
    if (ref) {
      const auto track = library.get_track(*ref, {});
      if (track.ok() && !track.value().genres.empty()) {
        (*result)["genre"] = track.value().genres.front();
      }
    }
    return jsonResponse(200, result->dump());
  });

  CROW_ROUTE(app, "/api/track/<int>").methods("DELETE"_method)([](int id) {
    repo::TrackRepo repo(config::get().db_path.string());
    if (repo.remove(id)) {
      return jsonResponse(200, "{\"status\":\"ok\"}");
    }
    return jsonResponse(404, "{\"erro\":true}");
  });

  CROW_ROUTE(app, "/api/album/<int>")([](int id) {
    Service service;
    auto result = service.albumById(id);
    if (!result) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    return jsonResponse(200, result->dump());
  });

  CROW_ROUTE(app, "/api/album/<int>").methods("DELETE"_method)([](int id) {
    try {
      repo::AlbumRepo album_repo(config::get().db_path.string());
      if (!album_repo.byId(id)) {
        return jsonResponse(404, "{\"erro\":true}");
      }
      SQLite::Database &db = db::conn(config::get().db_path.string());

      int removed = 0;
      {
        SQLite::Statement count(db,
                                "SELECT COUNT(*) FROM track WHERE album_id = ?");
        count.bind(1, id);
        if (count.executeStep()) {
          removed = count.getColumn(0).getInt();
        }
      }
      {
        SQLite::Statement del_tracks(db,
                                     "DELETE FROM track WHERE album_id = ?");
        del_tracks.bind(1, id);
        del_tracks.executeStep();
      }
      {
        SQLite::Statement del_album(db, schema::sql::kAlbumDelete);
        del_album.bind(1, id);
        del_album.executeStep();
      }

      crow::json::wvalue out;
      out["status"] = "ok";
      out["tracks_removed"] = removed;
      return jsonResponse(200, out.dump());
    } catch (const std::exception &e) {
      return jsonResponse(500, "{\"erro\":true}");
    }
  });

  CROW_ROUTE(app, "/api/album/<int>/update")
      .methods("POST"_method)([](const crow::request &req, int id) {
        try {
          repo::AlbumRepo album_repo(config::get().db_path.string());
          auto album = album_repo.byId(id);
          if (!album) {
            return jsonResponse(404, "{\"erro\":true}");
          }

          const crow::json::rvalue body = crow::json::load(req.body);
          if (!body) {
            return jsonResponse(400, "{\"erro\":true}");
          }

          SQLite::Database &db = db::conn(config::get().db_path.string());

          const std::string new_name =
              body.has("name") ? body["name"].s() : album->name;

          std::vector<std::string> artist_names;
          if (body.has("album_artist")) {
            artist_names = metadata::splitArtists(body["album_artist"].s());
          } else {
            repo::ArtistRepo artist_repo(config::get().db_path.string());
            for (const int aid : album->artist_ids) {
              if (auto a = artist_repo.byId(aid)) {
                artist_names.push_back(a->name);
              }
            }
          }
          const std::string key = artistKeyOf(artist_names);
          const std::vector<int> artist_ids = ensureArtistIds(db, artist_names);

          int existing = 0;
          {
            SQLite::Statement query(db, schema::sql::kAlbumByKey);
            query.bind(1, new_name);
            query.bind(2, key);
            if (query.executeStep()) {
              existing = query.getColumn(0).getInt();
            }
          }
          const int target = (existing != 0 && existing != id) ? existing : id;

          if (target != id) {
            SQLite::Statement reassign(db, schema::sql::kTrackReassignAlbum);
            reassign.bind(1, target);
            reassign.bind(2, id);
            reassign.executeStep();

            SQLite::Statement count(
                db, "SELECT COUNT(*) FROM track WHERE album_id = ?");
            count.bind(1, id);
            if (count.executeStep() && count.getColumn(0).getInt() == 0) {
              SQLite::Statement del(db, schema::sql::kAlbumDelete);
              del.bind(1, id);
              del.executeStep();
            }
          } else {
            SQLite::Statement update(db, schema::sql::kAlbumUpdateNameKey);
            update.bind(1, new_name);
            update.bind(2, key);
            update.bind(3, id);
            update.executeStep();
          }

          setAlbumArtists(db, target, artist_ids);

          if (body.has("year")) {
            SQLite::Statement update(db, schema::sql::kAlbumUpdateYear);
            update.bind(1, static_cast<int>(body["year"].i()));
            update.bind(2, target);
            update.executeStep();
          }
          if (body.has("cover_path")) {
            std::string cover_value = body["cover_path"].s();
            if (!cover_value.empty()) {
              repo::ScanRootRepo cover_roots(config::get().db_path.string());
              for (const auto &r : cover_roots.list()) {
                const std::string v = storage::toVirtual(r, cover_value);
                if (v != cover_value) {
                  cover_value = v;
                  break;
                }
              }
            }
            SQLite::Statement update(db, schema::sql::kAlbumUpdateCover);
            update.bind(1, cover_value);
            update.bind(2, target);
            update.executeStep();
          }

          crow::json::wvalue out;
          out["status"] = "ok";
          out["album_id"] = target;
          out["merged"] = (target != id);
          return jsonResponse(200, out.dump());
        } catch (const std::exception &) {
          return jsonResponse(409, "{\"erro\":true,\"reason\":\"conflict\"}");
        }
      });

  CROW_ROUTE(app, "/api/artist/<int>")([](int id) {
    Service service;
    auto result = service.artistById(id);
    if (!result) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    return jsonResponse(200, result->dump());
  });

  CROW_ROUTE(app, "/api/album-artists")([] {
    const auto snap = media_library::library().snapshot();
    struct Row {
      int id = 0;
      std::string name;
      int album_count = 0;
    };
    std::vector<Row> rows;
    for (std::size_t i = 0; i < snap.artists.size(); ++i) {
      const int internal_id = snap.artist_internal_ids[i];
      const auto it = snap.artist_album_count.find(internal_id);
      const int count = it == snap.artist_album_count.end() ? 0 : it->second;
      if (count <= 0) {
        continue;
      }
      rows.push_back(Row{internal_id, snap.artists[i].name, count});
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row &a, const Row &b) { return a.name < b.name; });

    crow::json::wvalue out(crow::json::type::List);
    int i = 0;
    for (const auto &row : rows) {
      out[i]["id"] = row.id;
      out[i]["name"] = row.name;
      out[i]["album_count"] = row.album_count;
      ++i;
    }
    return jsonResponse(200, out.dump());
  });

  CROW_ROUTE(app, "/api/library/search")([](const crow::request &req) {
    std::string q;
    if (const char *v = req.url_params.get("q")) {
      q = v;
    }
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    q.erase(q.begin(), std::find_if(q.begin(), q.end(), notSpace));
    q.erase(std::find_if(q.rbegin(), q.rend(), notSpace).base(), q.end());

    crow::json::wvalue out;
    crow::json::wvalue::list empty_artists;
    if (q.empty()) {
      out["artists"] = crow::json::wvalue(empty_artists);
      return jsonResponse(200, out.dump());
    }

    auto &library = media_library::library();
    const auto snap = library.snapshot();
    const std::string needle = media_library_lower(q);

    std::unordered_map<int, std::string> artist_names;
    for (std::size_t i = 0; i < snap.artists.size(); ++i) {
      artist_names[snap.artist_internal_ids[i]] = snap.artists[i].name;
    }

    std::unordered_map<int, std::size_t> album_index;
    for (std::size_t i = 0; i < snap.albums.size(); ++i) {
      album_index[snap.album_internal_ids[i]] = i;
    }

    // 专辑内部 ID -> 按碟号/曲目号排序的曲目下标
    std::map<int, std::vector<std::size_t>> album_tracks;
    for (std::size_t i = 0; i < snap.tracks.size(); ++i) {
      const auto &track = snap.tracks[i];
      if (!track.album) {
        continue;
      }
      const auto album_id = library.internal_id_of(track.album->ref);
      if (!album_id) {
        continue;
      }
      album_tracks[*album_id].push_back(i);
    }
    for (auto &entry : album_tracks) {
      std::stable_sort(entry.second.begin(), entry.second.end(),
                       [&snap](std::size_t a, std::size_t b) {
                         const auto &left = snap.tracks[a];
                         const auto &right = snap.tracks[b];
                         const int ad = left.disc_no.value_or(0);
                         const int bd = right.disc_no.value_or(0);
                         if (ad != bd) {
                           return ad < bd;
                         }
                         const int at = left.track_no.value_or(0);
                         const int bt = right.track_no.value_or(0);
                         if (at != bt) {
                           return at < bt;
                         }
                         return left.title < right.title;
                       });
    }

    // 命中的专辑：专辑名或其中曲目名包含关键词
    std::set<int> album_ids;
    for (std::size_t i = 0; i < snap.albums.size(); ++i) {
      const auto &album = snap.albums[i];
      const int album_id = snap.album_internal_ids[i];
      if (!album.title.empty() &&
          media_library_lower(album.title).find(needle) != std::string::npos) {
        album_ids.insert(album_id);
        continue;
      }
      const auto it = album_tracks.find(album_id);
      if (it == album_tracks.end()) {
        continue;
      }
      for (const std::size_t index : it->second) {
        if (media_library_lower(snap.tracks[index].title).find(needle) !=
            std::string::npos) {
          album_ids.insert(album_id);
          break;
        }
      }
    }

    // 命中的艺术家：名称包含关键词且至少有一张专辑
    std::set<int> matched_artists;
    for (std::size_t i = 0; i < snap.artists.size(); ++i) {
      const int artist_id = snap.artist_internal_ids[i];
      const auto count = snap.artist_album_count.find(artist_id);
      if (count == snap.artist_album_count.end() || count->second <= 0) {
        continue;
      }
      if (media_library_lower(snap.artists[i].name).find(needle) !=
          std::string::npos) {
        matched_artists.insert(artist_id);
      }
    }

    std::map<int, std::set<int>> by_artist;
    for (const int album_id : album_ids) {
      const auto index = album_index.find(album_id);
      if (index == album_index.end()) {
        continue;
      }
      bool any = false;
      for (const auto &link : snap.albums[index->second].artists) {
        if (const auto artist_id = library.internal_id_of(link.ref)) {
          by_artist[*artist_id].insert(album_id);
          any = true;
        }
      }
      if (!any) {
        by_artist[0].insert(album_id);
      }
    }
    for (const int artist_id : matched_artists) {
      for (std::size_t i = 0; i < snap.albums.size(); ++i) {
        const int album_id = snap.album_internal_ids[i];
        for (const auto &link : snap.albums[i].artists) {
          const auto linked = library.internal_id_of(link.ref);
          if (linked && *linked == artist_id) {
            by_artist[artist_id].insert(album_id);
            break;
          }
        }
      }
    }

    crow::json::wvalue::list artists_json;
    for (const auto &kv : by_artist) {
      crow::json::wvalue aj;
      aj["id"] = kv.first;
      if (kv.first == 0) {
        aj["name"] = std::string("(No album artist)");
      } else {
        const auto name = artist_names.find(kv.first);
        aj["name"] = name == artist_names.end() ? std::string() : name->second;
      }

      std::vector<int> albums(kv.second.begin(), kv.second.end());
      std::sort(albums.begin(), albums.end(), [&](int x, int y) {
        const auto xi = album_index.find(x);
        const auto yi = album_index.find(y);
        const std::string nx =
            xi == album_index.end() ? std::string()
                                    : snap.albums[xi->second].title;
        const std::string ny =
            yi == album_index.end() ? std::string()
                                    : snap.albums[yi->second].title;
        return nx < ny;
      });

      crow::json::wvalue::list albums_json;
      for (const int album_id : albums) {
        const auto index = album_index.find(album_id);
        if (index == album_index.end()) {
          continue;
        }
        const auto &album = snap.albums[index->second];
        crow::json::wvalue alj;
        alj["id"] = album_id;
        alj["name"] = album.title;
        alj["year"] = album.year.value_or(0);

        crow::json::wvalue::list tracks_json;
        const auto it = album_tracks.find(album_id);
        if (it != album_tracks.end()) {
          for (const std::size_t index : it->second) {
            const auto &track = snap.tracks[index];
            const auto track_id = library.internal_id_of(track.ref);
            if (!track_id) {
              continue;
            }
            crow::json::wvalue tj;
            tj["id"] = *track_id;
            tj["title"] = track.title;
            tj["track_no"] = track.track_no.value_or(0);
            tj["duration_ms"] = track.duration_ms.value_or(0);
            tracks_json.push_back(std::move(tj));
          }
        }
        alj["tracks"] = crow::json::wvalue(tracks_json);
        albums_json.push_back(std::move(alj));
      }
      aj["albums"] = crow::json::wvalue(albums_json);
      artists_json.push_back(std::move(aj));
    }
    out["artists"] = crow::json::wvalue(artists_json);
    return jsonResponse(200, out.dump());
  });

  CROW_ROUTE(app, "/api/playlists/refresh").methods("POST"_method)([] {
    auto r = playlist::refresh();
    crow::json::wvalue out;
    out["playlists"] = r.playlists;
    out["songs"] = r.songs;
    out["matched"] = r.matched;
    crow::json::wvalue::list unmatched;
    for (const auto &u : r.unmatched) {
      crow::json::wvalue uw;
      uw["playlist"] = u.playlist;
      uw["path"] = u.song.path;
      uw["album"] = u.song.album;
      uw["album_artist"] = u.song.album_artist;
      unmatched.emplace_back(std::move(uw));
    }
    out["unmatched"] = crow::json::wvalue(unmatched);
    return jsonResponse(200, out.dump());
  });

  CROW_ROUTE(app, "/api/playlists")([] {
    auto playlists = playlist::list();
    crow::json::wvalue out(crow::json::type::List);
    int i = 0;
    for (const auto &p : playlists) {
      out[i] = playlistSummaryJson(p);
      ++i;
    }
    return jsonResponse(200, out.dump());
  });

  CROW_ROUTE(app, "/api/playlists")
      .methods("POST"_method)([](const crow::request &req) {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("name")) {
          return jsonResponse(400, "{\"erro\":true}");
        }
        playlist::Playlist p = playlist::create(std::string(json["name"].s()));
        return jsonResponse(200, playlistDetailJson(p).dump());
      });

  CROW_ROUTE(app, "/api/playlists/<string>")([](const crow::request &,
                                                std::string id) {
    playlist::Playlist p;
    if (!playlist::get(id, p)) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    return jsonResponse(200, playlistDetailJson(p).dump());
  });

  CROW_ROUTE(app, "/api/playlists/<string>")
      .methods("DELETE"_method)([](const crow::request &, std::string id) {
        if (!playlist::remove(id)) {
          return jsonResponse(404, "{\"erro\":true}");
        }
        return jsonResponse(200, "{\"status\":\"ok\"}");
      });

  CROW_ROUTE(app, "/api/playlists/<string>/songs")
      .methods("POST"_method)([](const crow::request &req, std::string id) {
        auto json = crow::json::load(req.body);
        if (!json) {
          return jsonResponse(400, "{\"erro\":true}");
        }
        std::string err;
        bool ok = false;
        if (json.has("trackId")) {
          ok = playlist::addTrack(id, static_cast<int>(json["trackId"].i()), err);
        } else if (json.has("path")) {
          ok = playlist::addPath(id, std::string(json["path"].s()), err);
        } else {
          err = "missing path or trackId";
        }
        if (!ok) {
          return jsonResponse(400, "{\"erro\":true,\"reason\":\"" + err + "\"}");
        }
        playlist::Playlist p;
        playlist::get(id, p);
        return jsonResponse(200, playlistDetailJson(p).dump());
      });

  CROW_ROUTE(app, "/api/playlists/<string>/songs/<int>")
      .methods("DELETE"_method)([](const crow::request &, std::string id,
                                   int index) {
        if (!playlist::removeAt(id, index)) {
          return jsonResponse(404, "{\"erro\":true}");
        }
        return jsonResponse(200, "{\"status\":\"ok\"}");
      });

  CROW_ROUTE(app, "/api/roots")([] {
    repo::ScanRootRepo repo(config::get().db_path.string());
    auto roots = repo.list();
    crow::json::wvalue out(crow::json::type::List);
    int i = 0;
    for (const auto &r : roots) {
      out[i]["id"] = r.id;
      out[i]["path"] = r.path;
      out[i]["added_at"] = r.added_at;
      out[i]["last_scan"] = r.last_scan;
      out[i]["name"] = r.name;
      out[i]["type"] = r.type;
      ++i;
    }
    return jsonResponse(200, out.dump());
  });

  CROW_ROUTE(app, "/api/roots")
      .methods("POST"_method)([](const crow::request &req) {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("path")) {
          return jsonResponse(400, "{\"erro\":true}");
        }
        repo::ScanRootRepo repo(config::get().db_path.string());
        const std::string path = json["path"].s();
        const std::string user =
            json.has("user") ? std::string(json["user"].s()) : std::string();
        const std::string pass =
            json.has("pass") ? std::string(json["pass"].s()) : std::string();
        std::string name =
            json.has("name") ? std::string(json["name"].s()) : std::string();
        if (name.empty()) {
          name = deriveRootName(path);
        }
        std::string type =
            json.has("type") ? std::string(json["type"].s()) : std::string();
        if (type.empty()) {
          type = rootTypeOf(path);
        }
        int id = repo.add(path, user, pass, name, type);
        crow::json::wvalue out;
        out["id"] = id;
        return jsonResponse(200, out.dump());
      });

  CROW_ROUTE(app, "/api/roots/<int>")
      .methods("DELETE"_method)([](int id) {
        repo::ScanRootRepo repo(config::get().db_path.string());
        if (!repo.byId(id)) {
          return jsonResponse(404, "{\"erro\":true}");
        }
        SQLite::Database &db = db::conn(config::get().db_path.string());
        int removed = 0;
        try {
          SQLite::Statement del(db, "DELETE FROM track WHERE root_id = ?");
          del.bind(1, id);
          del.executeStep();
          removed = del.getChanges();
          repo.remove(id);
          db.exec("DELETE FROM album WHERE id NOT IN "
                  "(SELECT DISTINCT album_id FROM track WHERE album_id IS NOT NULL)");
          db.exec("DELETE FROM artist WHERE id NOT IN "
                  "(SELECT DISTINCT artist_id FROM track_artist) AND id NOT IN "
                  "(SELECT DISTINCT artist_id FROM album_artist)");
        } catch (const std::exception &) {
          return jsonResponse(500, "{\"erro\":true}");
        }
        crow::json::wvalue out;
        out["status"] = "ok";
        out["tracks_removed"] = removed;
        return jsonResponse(200, out.dump());
      });

  CROW_ROUTE(app, "/api/scan")
      .methods("POST"_method)([](const crow::request &) {
        const int job_id = scanner::enqueueJob(
            "scan-all", [] { scanner::scanAllBlocking(); });
        crow::json::wvalue out;
        out["status"] = "queued";
        out["jobId"] = job_id;
        return jsonResponse(202, out.dump());
      });

  CROW_ROUTE(app, "/api/scan/status")([] {
    const auto p = scanner::status();
    crow::json::wvalue out;
    out["scanning"] = p.scanning;
    out["total_roots"] = p.total_roots;
    out["current_root_index"] = p.current_root_index;
    out["roots"] = crow::json::wvalue(crow::json::type::List);
    for (size_t i = 0; i < p.roots.size(); ++i) {
      const auto &r = p.roots[i];
      out["roots"][i]["id"] = r.id;
      out["roots"][i]["path"] = r.path;
      out["roots"][i]["scanning"] = r.scanning;
      out["roots"][i]["files_processed"] = r.files_processed;
      out["roots"][i]["files_added"] = r.files_added;
      out["roots"][i]["files_updated"] = r.files_updated;
      out["roots"][i]["files_removed"] = r.files_removed;
      out["roots"][i]["errors"] = r.errors;
      out["roots"][i]["current_file"] = r.current_file;
    }
    return jsonResponse(200, out.dump());
  });

  CROW_ROUTE(app, "/api/jobs")([] {
    const auto jobs = scanner::jobList();
    crow::json::wvalue out(crow::json::type::List);
    int i = 0;
    for (const auto &j : jobs) {
      crow::json::wvalue item;
      item["id"] = j.id;
      item["kind"] = j.kind;
      item["state"] = j.state;
      item["processed"] = j.processed;
      item["added"] = j.added;
      item["updated"] = j.updated;
      item["removed"] = j.removed;
      item["errors"] = j.errors;
      item["current"] = j.current;
      item["threads"] = j.threads;
      crow::json::wvalue::list paths;
      for (const auto &p : j.current_paths) {
        paths.emplace_back(p);
      }
      item["current_paths"] = crow::json::wvalue(std::move(paths));
      item["error"] = j.error;
      out[i] = std::move(item);
      ++i;
    }
    return jsonResponse(200, out.dump());
  });

  CROW_ROUTE(app, "/api/jobs/<int>")([](int id) {
    const auto j = scanner::jobStatus(id);
    if (j.id == 0) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    crow::json::wvalue out;
    out["id"] = j.id;
    out["kind"] = j.kind;
    out["state"] = j.state;
    out["processed"] = j.processed;
    out["added"] = j.added;
    out["updated"] = j.updated;
    out["removed"] = j.removed;
    out["errors"] = j.errors;
    out["current"] = j.current;
    out["threads"] = j.threads;
    crow::json::wvalue::list paths;
    for (const auto &p : j.current_paths) {
      paths.emplace_back(p);
    }
    out["current_paths"] = crow::json::wvalue(std::move(paths));
    out["error"] = j.error;
    return jsonResponse(200, out.dump());
  });

  CROW_ROUTE(app, "/api/recent")([] {
    const auto snap = media_library::library().snapshot();
    crow::json::wvalue out(crow::json::type::List);
    int i = 0;
    for (const int internal_id : snap.recent_play_track_ids) {
      auto item = Service().trackById(internal_id);
      if (!item) {
        continue;
      }
      out[i] = std::move(*item);
      ++i;
      if (config::get().page_size > 0 &&
          i >= config::get().page_size) {
        break;
      }
    }
    return jsonResponse(200, out.dump());
  });

  CROW_ROUTE(app, "/api/album/<int>/cover")([](int id) {
    auto &library = media_library::library();
    const auto ref =
        library.ref_for_compat(media_library::EntityType::Album, id);
    if (!ref) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    const auto cover_path = library.artwork_path(*ref);
    if (!cover_path || cover_path->empty()) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    const std::string cover_real = storage::resolve(*cover_path);
    if (!webdav::isWebdavUrl(cover_real)) {
      fs::path cover(cover_real);
      if (!fs::exists(cover) || !isUnderAnyRoot(cover)) {
        return jsonResponse(404, "{\"erro\":true}");
      }
    }
    return http_util::serveImage(*cover_path);
  });

  CROW_ROUTE(app, "/api/track/<int>/file")([](const crow::request &req, int id) {
    auto &library = media_library::library();
    const auto ref =
        library.ref_for_compat(media_library::EntityType::Track, id);
    if (!ref) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    const auto source = library.resolve_playback(*ref, {});
    if (!source.ok() || source.value().internal_locator.empty()) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    const std::string track_real = source.value().internal_locator;
    if (webdav::isWebdavUrl(track_real)) {
      return http_util::serveResource(track_real,
                                      req.get_header_value("Range"));
    }
    fs::path track_path(track_real);
    if (!fs::exists(track_path) || !isUnderAnyRoot(track_path)) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    return serveFile(track_path, req.get_header_value("Range"));
  });

  CROW_ROUTE(app, "/api/track/<int>/play")
      .methods("POST"_method)([](int id) {
        repo::TrackRepo repo(config::get().db_path.string());
        if (repo.recordPlay(id)) {
          return jsonResponse(200, "{\"status\":\"ok\"}");
        }
        return jsonResponse(404, "{\"erro\":true}");
      });

  CROW_ROUTE(app, "/api/fs/browse/<int>")([](int root_id) {
    return browseDir(root_id, "");
  });

  CROW_ROUTE(app, "/api/fs/browse/<int>/<path>")([](int root_id, std::string path) {
    return browseDir(root_id, urlDecode(path));
  });

  CROW_ROUTE(app, "/api/fs/file-meta/<path>")([](std::string path) {
    const std::string p = urlDecode(path);
    if (!webdav::isWebdavUrl(p) && (!isUnderAnyRoot(p) || !fs::exists(p))) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    auto meta = metadata::readFileMeta(p);
    if (!meta) {
      return jsonResponse(500, "{\"erro\":true}");
    }
    crow::json::wvalue out;
    out["title"] = meta->title;
    out["artist"] = meta->artist;
    out["album"] = meta->album;
    out["genre"] = meta->genre;
    out["year"] = meta->year;
    out["comment"] = meta->comment;
    out["track_no"] = meta->track_no;
    out["disc_no"] = meta->disc_no;
    out["size_bytes"] = static_cast<long long>(meta->size_bytes);
    out["bitrate"] = meta->bitrate;
    out["sample_rate"] = meta->sample_rate;
    out["channels"] = meta->channels;
    out["format"] = meta->format;
    crow::json::wvalue::list artists;
    for (const auto &name : meta->artists) {
      artists.emplace_back(name);
    }
    out["artists"] = crow::json::wvalue(artists);
    crow::json::wvalue::list album_artists;
    for (const auto &name : meta->album_artists) {
      album_artists.emplace_back(name);
    }
    out["album_artists"] = crow::json::wvalue(album_artists);
    return jsonResponse(200, out.dump());
  });

  CROW_ROUTE(app, "/api/fs/orphans")([](const crow::request &req) {
    try {
      int root_id = 0;
      std::string root_id_str = req.url_params.get("rootId");
      if (!root_id_str.empty()) {
        root_id = std::stoi(root_id_str);
      }
      if (root_id <= 0) {
        return jsonResponse(400, "{\"erro\":true}");
      }

      repo::ScanRootRepo root_repo(config::get().db_path.string());
      auto root_opt = root_repo.byId(root_id);
      if (!root_opt) {
        return jsonResponse(404, "{\"erro\":true}");
      }

      repo::TrackRepo track_repo(config::get().db_path.string());
      auto tracks = track_repo.byRootId(root_id);

      crow::json::wvalue out(crow::json::type::List);
      int i = 0;
      for (const auto &t : tracks) {
        if (!fs::exists(t.path)) {
          out[i]["id"] = t.id;
          out[i]["path"] = t.path;
          out[i]["title"] = t.title;
          out[i]["album_id"] = t.album_id;
          crow::json::wvalue::list aids;
          for (const int aid : t.artist_ids) {
            aids.emplace_back(aid);
          }
          out[i]["artist_ids"] = crow::json::wvalue(aids);
          ++i;
        }
      }
      return jsonResponse(200, out.dump());
    } catch (...) {
      return jsonResponse(500, "{\"erro\":true}");
    }
  });

  CROW_ROUTE(app, "/api/track/<int>/file-meta")([](int id) {
    auto &library = media_library::library();
    const auto ref =
        library.ref_for_compat(media_library::EntityType::Track, id);
    if (!ref) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    const auto source = library.resolve_playback(*ref, {});
    if (!source.ok() || source.value().internal_locator.empty()) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    const std::string track_real = source.value().internal_locator;
    if (!webdav::isWebdavUrl(track_real) && !fs::exists(track_real)) {
      return jsonResponse(404, "{\"erro\":true}");
    }

    auto meta = metadata::readFileMeta(track_real);
    if (!meta) {
      return jsonResponse(500, "{\"erro\":true}");
    }

    crow::json::wvalue out;
    out["title"] = meta->title;
    out["artist"] = meta->artist;
    out["album"] = meta->album;
    out["genre"] = meta->genre;
    out["year"] = meta->year;
    out["comment"] = meta->comment;
    out["track_no"] = meta->track_no;
    out["disc_no"] = meta->disc_no;
    out["size_bytes"] = static_cast<long long>(meta->size_bytes);
    out["bitrate"] = meta->bitrate;
    out["sample_rate"] = meta->sample_rate;
    out["channels"] = meta->channels;
    out["format"] = meta->format;
    crow::json::wvalue::list artists;
    for (const auto &name : meta->artists) {
      artists.emplace_back(name);
    }
    out["artists"] = crow::json::wvalue(artists);
    crow::json::wvalue::list album_artists;
    for (const auto &name : meta->album_artists) {
      album_artists.emplace_back(name);
    }
    out["album_artists"] = crow::json::wvalue(album_artists);
    return jsonResponse(200, out.dump());
  });

  CROW_ROUTE(app, "/api/track/<int>/lyrics")([](int id) {
    auto &library = media_library::library();
    const auto ref =
        library.ref_for_compat(media_library::EntityType::Track, id);
    if (!ref) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    crow::json::wvalue out;
    out["text"] = "";
    out["synced"] = false;
    const auto lyrics = library.get_lyrics(*ref, {});
    if (lyrics.ok()) {
      const auto &value = lyrics.value();
      out["text"] = value.plain_text.value_or(std::string());
      out["synced"] = value.mode != media_library::LyricsMode::Unsynced;
    }
    return jsonResponse(200, out.dump());
  });

  CROW_ROUTE(app, "/api/track/<int>/lyrics")
      .methods("POST"_method)([](const crow::request &req, int id) {
        repo::TrackRepo repo(config::get().db_path.string());
        if (!repo.byId(id)) {
          return jsonResponse(404, "{\"erro\":true}");
        }
        auto body = crow::json::load(req.body);
        if (!body || !body.has("text")) {
          return jsonResponse(400, "{\"erro\":true}");
        }
        const std::string text = body["text"].s();
        SQLite::Database &db = db::conn(config::get().db_path.string());
        if (text.empty()) {
          SQLite::Statement del(db,
                                "DELETE FROM track_lyrics WHERE track_id = ?");
          del.bind(1, id);
          del.executeStep();
        } else {
          SQLite::Statement up(db, schema::sql::kTrackLyricsUpsert);
          up.bind(1, id);
          up.bind(2, text);
          up.bind(3, metadata::isSyncedLyrics(text) ? 1 : 0);
          up.executeStep();
        }
        return jsonResponse(200, "{\"status\":\"ok\"}");
      });

  CROW_ROUTE(app, "/api/scan/dir")
      .methods("POST"_method)([](const crow::request &req) {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("rootId") || !json.has("path")) {
          return jsonResponse(400, "{\"erro\":true}");
        }
        int root_id = json["rootId"].i();
        std::string dir_path = json["path"].s();

        repo::ScanRootRepo root_repo(config::get().db_path.string());
        auto root_opt = root_repo.byId(root_id);
        if (!root_opt) {
          return jsonResponse(404, "{\"erro\":true}");
        }

        std::string target = dir_path;
        if (!webdav::isWebdavUrl(dir_path) &&
            !webdav::isWebdavUrl(root_opt->path)) {
          std::error_code ec;
          fs::path full_path = fs::weakly_canonical(root_opt->path) / dir_path;
          if (!fs::is_directory(full_path, ec) &&
              !fs::is_regular_file(full_path, ec)) {
            return jsonResponse(400, "{\"erro\":true}");
          }
          target = full_path.string();
        }

        const int job_id = scanner::enqueueJob(
            "scan-dir",
            [root_id, target] { scanner::scanDir(root_id, target); });
        crow::json::wvalue out;
        out["status"] = "queued";
        out["jobId"] = job_id;
        return jsonResponse(202, out.dump());
      });

  CROW_ROUTE(app, "/api/track/<int>/sync-from-file")
      .methods("POST"_method)([](int id) {
        repo::TrackRepo track_repo(config::get().db_path.string());
        if (!track_repo.byId(id)) {
          return jsonResponse(404, "{\"erro\":true}");
        }
        const int job_id = scanner::enqueueJob(
            "sync-from-file", [id] { syncFromFileById(id); });
        crow::json::wvalue out;
        out["status"] = "queued";
        out["jobId"] = job_id;
        return jsonResponse(202, out.dump());
      });

  CROW_ROUTE(app, "/api/track/<int>/sync-to-file")
      .methods("POST"_method)([](int id) {
        repo::TrackRepo track_repo(config::get().db_path.string());
        if (!track_repo.byId(id)) {
          return jsonResponse(404, "{\"erro\":true}");
        }
        const int job_id = scanner::enqueueJob(
            "sync-to-file", [id] { syncToFileById(id); });
        crow::json::wvalue out;
        out["status"] = "queued";
        out["jobId"] = job_id;
        return jsonResponse(202, out.dump());
      });

  CROW_ROUTE(app, "/api/track/<int>/update")
      .methods("POST"_method)([](const crow::request &req, int id) {
        auto json = crow::json::load(req.body);
        if (!json) {
          return jsonResponse(400, "{\"erro\":true}");
        }

        repo::TrackRepo repo(config::get().db_path.string());
        auto track = repo.byId(id);
        if (!track) {
          return jsonResponse(404, "{\"erro\":true}");
        }

        repo::Track t = *track;
        if (json.has("title")) {
          t.title = json["title"].s();
        }
        SQLite::Database &db = db::conn(config::get().db_path.string());
        repo::ArtistRepo artist_repo(config::get().db_path.string());
        repo::AlbumRepo album_repo(config::get().db_path.string());

        std::vector<std::string> artist_names;
        if (json.has("artist")) {
          artist_names = metadata::splitArtists(json["artist"].s());
        }

        std::vector<std::string> album_artist_names;
        if (json.has("album_artist")) {
          album_artist_names = metadata::splitArtists(json["album_artist"].s());
        } else if (!artist_names.empty()) {
          album_artist_names = artist_names;
        }

        std::string album_name;
        if (json.has("album")) {
          album_name = json["album"].s();
        }

        if (!artist_names.empty()) {
          setTrackArtists(db, id, ensureArtistIds(db, artist_names));
        }

        if (json.has("album") || json.has("album_artist")) {
          if (album_name.empty() && t.album_id > 0) {
            if (auto a = album_repo.byId(t.album_id)) {
              album_name = a->name;
            }
          }
          if (album_artist_names.empty() && t.album_id > 0) {
            for (const int aid : album_repo.artistIdsByAlbumId(t.album_id)) {
              if (auto a = artist_repo.byId(aid)) {
                album_artist_names.push_back(a->name);
              }
            }
          }
          const int album_id =
              ensureAlbumId(db, album_name, artistKeyOf(album_artist_names));
          if (album_id != 0) {
            t.album_id = album_id;
            setAlbumArtists(db, album_id,
                            ensureArtistIds(db, album_artist_names));
          }
        }
        if (json.has("year") && t.album_id > 0) {
          SQLite::Statement update(db, schema::sql::kAlbumUpdateYear);
          update.bind(1, json["year"].i());
          update.bind(2, t.album_id);
          update.executeStep();
        }
        if (json.has("genre")) {
          std::string genre_name = json["genre"].s();
          SQLite::Statement del(db,
                                "DELETE FROM track_genre WHERE track_id = ?");
          del.bind(1, id);
          del.executeStep();
          if (!genre_name.empty()) {
            SQLite::Statement ins(db, schema::sql::kGenreInsert);
            ins.bind(1, genre_name);
            ins.executeStep();
            SQLite::Statement query(db, schema::sql::kGenreByName);
            query.bind(1, genre_name);
            if (query.executeStep()) {
              SQLite::Statement link(db, schema::sql::kTrackGenreInsert);
              link.bind(1, id);
              link.bind(2, query.getColumn(0).getInt());
              link.executeStep();
            }
          }
        }
        if (json.has("disc_no")) {
          t.disc_no = json["disc_no"].i();
        }
        if (json.has("track_no")) {
          t.track_no = json["track_no"].i();
        }
        if (json.has("duration_ms")) {
          t.duration_ms = json["duration_ms"].i();
        }
        if (json.has("bitrate")) {
          t.bitrate = json["bitrate"].i();
        }
        if (json.has("sample_rate")) {
          t.sample_rate = json["sample_rate"].i();
        }
        if (json.has("channels")) {
          t.channels = json["channels"].i();
        }
        if (json.has("format")) {
          t.format = json["format"].s();
        }
        if (json.has("size_bytes")) {
          t.size_bytes = json["size_bytes"].i();
        }
        if (json.has("mtime")) {
          t.mtime = json["mtime"].i();
        }
        if (json.has("rating")) {
          t.rating = json["rating"].i();
        }

        repo.updatePartial(t);
        return jsonResponse(200, "{\"status\":\"ok\"}");
      });

  CROW_ROUTE(app, "/api/fs/batch")
      .methods("POST"_method)([](const crow::request &req) {
        auto json = crow::json::load(req.body);
        if (!json) {
          return jsonResponse(400, "{\"erro\":true}");
        }
        bool is_list = false;
        try { is_list = json.t() == crow::json::type::List; } catch (...) {}
        if (!is_list) {
          return jsonResponse(400, "{\"erro\":true}");
        }

        crow::json::wvalue result(crow::json::type::List);
        std::map<int, std::vector<std::string>> scan_paths_by_root;
        std::map<int, std::vector<int>> scan_indices_by_root;
        std::vector<int> sync_from_ids;
        std::vector<int> sync_from_indices;
        std::vector<int> sync_to_ids;
        std::vector<int> sync_to_indices;
        const auto collectTrackIds = [](const crow::json::rvalue &item,
                                        std::vector<int> &out) {
          if (item.has("trackId")) {
            out.push_back(item["trackId"].i());
            return;
          }
          if (item.has("albumId")) {
            for (const int id : tracksUnderAlbum(item["albumId"].i())) {
              out.push_back(id);
            }
            return;
          }
          if (item.has("artistId")) {
            for (const int id : tracksUnderArtist(item["artistId"].i())) {
              out.push_back(id);
            }
            return;
          }
          if (item.has("path")) {
            const int root_id = item["rootId"].i();
            for (const int id : tracksUnderPath(root_id, item["path"].s())) {
              out.push_back(id);
            }
          }
        };
        int i = 0;
        for (const auto &item : json) {
          std::string action = item["action"].s();
          if (action == "scan") {
            const int root_id = item["rootId"].i();
            scan_paths_by_root[root_id].push_back(item["path"].s());
            scan_indices_by_root[root_id].push_back(i);
          } else if (action == "sync-from-file") {
            collectTrackIds(item, sync_from_ids);
            sync_from_indices.push_back(i);
          } else if (action == "sync-to-file") {
            collectTrackIds(item, sync_to_ids);
            sync_to_indices.push_back(i);
          } else if (action == "update") {
            int track_id = item["trackId"].i();
            auto fields = item["fields"];
            repo::TrackRepo track_repo(config::get().db_path.string());
            auto track = track_repo.byId(track_id);
            if (track) {
              repo::Track t = *track;
              if (fields.has("title")) {
                t.title = fields["title"].s();
              }
              SQLite::Database &db = db::conn(config::get().db_path.string());
              repo::ArtistRepo artist_repo(config::get().db_path.string());
              repo::AlbumRepo album_repo(config::get().db_path.string());
              std::vector<std::string> artist_names;
              if (fields.has("artist")) {
                artist_names = metadata::splitArtists(fields["artist"].s());
              }
              std::vector<std::string> album_artist_names;
              if (fields.has("album_artist")) {
                album_artist_names =
                    metadata::splitArtists(fields["album_artist"].s());
              } else if (!artist_names.empty()) {
                album_artist_names = artist_names;
              }
              std::string album_name;
              if (fields.has("album")) {
                album_name = fields["album"].s();
              }
              if (!artist_names.empty()) {
                setTrackArtists(db, track_id, ensureArtistIds(db, artist_names));
              }
              if (fields.has("album") || fields.has("album_artist")) {
                if (album_name.empty() && t.album_id > 0) {
                  if (auto a = album_repo.byId(t.album_id)) {
                    album_name = a->name;
                  }
                }
                if (album_artist_names.empty() && t.album_id > 0) {
                  for (const int aid :
                       album_repo.artistIdsByAlbumId(t.album_id)) {
                    if (auto a = artist_repo.byId(aid)) {
                      album_artist_names.push_back(a->name);
                    }
                  }
                }
                const int album_id = ensureAlbumId(
                    db, album_name, artistKeyOf(album_artist_names));
                if (album_id != 0) {
                  t.album_id = album_id;
                  setAlbumArtists(db, album_id,
                                  ensureArtistIds(db, album_artist_names));
                }
              }
              if (fields.has("disc_no")) {
                t.disc_no = fields["disc_no"].i();
              }
              if (fields.has("track_no")) {
                t.track_no = fields["track_no"].i();
              }
              if (fields.has("duration_ms")) {
                t.duration_ms = fields["duration_ms"].i();
              }
              if (fields.has("bitrate")) {
                t.bitrate = fields["bitrate"].i();
              }
              if (fields.has("sample_rate")) {
                t.sample_rate = fields["sample_rate"].i();
              }
              if (fields.has("channels")) {
                t.channels = fields["channels"].i();
              }
              if (fields.has("format")) {
                t.format = fields["format"].s();
              }
              if (fields.has("size_bytes")) {
                t.size_bytes = fields["size_bytes"].i();
              }
              if (fields.has("mtime")) {
                t.mtime = fields["mtime"].i();
              }
              if (fields.has("rating")) {
                t.rating = fields["rating"].i();
              }
              track_repo.updatePartial(t);
              result[i]["status"] = "ok";
            } else {
              result[i]["status"] = "error";
              result[i]["error"] = "track not found";
            }
          } else if (action == "delete-index") {
            if (item.has("trackId") || item.has("albumId") ||
                item.has("artistId")) {
              SQLite::Database &db = db::conn(config::get().db_path.string());
              int removed = 0;
              try {
                if (item.has("trackId")) {
                  SQLite::Statement q(db, "DELETE FROM track WHERE id = ?");
                  q.bind(1, item["trackId"].i());
                  q.executeStep();
                  removed = q.getChanges();
                } else if (item.has("albumId")) {
                  SQLite::Statement q(db, "DELETE FROM track WHERE album_id = ?");
                  q.bind(1, item["albumId"].i());
                  q.executeStep();
                  removed = q.getChanges();
                } else {
                  SQLite::Statement q(
                      db,
                      "DELETE FROM track WHERE album_id IN "
                      "(SELECT album_id FROM album_artist WHERE artist_id = ?)");
                  q.bind(1, item["artistId"].i());
                  q.executeStep();
                  removed = q.getChanges();
                }
                db.exec("DELETE FROM album WHERE id NOT IN (SELECT DISTINCT "
                        "album_id FROM track WHERE album_id IS NOT NULL)");
                db.exec("DELETE FROM artist WHERE id NOT IN (SELECT DISTINCT "
                        "artist_id FROM track_artist) AND id NOT IN "
                        "(SELECT DISTINCT artist_id FROM album_artist)");
                result[i]["status"] = "ok";
                result[i]["tracks_removed"] = removed;
              } catch (...) {
                result[i]["status"] = "error";
                result[i]["error"] = "delete failed";
              }
              ++i;
              continue;
            }
            int root_id = item["rootId"].i();
            std::string path = item["path"].s();
            try {
              repo::ScanRootRepo root_repo(config::get().db_path.string());
              auto root_opt = root_repo.byId(root_id);
              if (!root_opt) {
                result[i]["status"] = "error";
                result[i]["error"] = "root not found";
              } else {
                std::error_code ec;
                const bool is_webdav = webdav::isWebdavUrl(path);
                std::string target;
                std::string root_canon;
                bool inside = false;
                if (is_webdav) {
                  target = path;
                  webdav::Url target_url;
                  webdav::Url root_url;
                  if (webdav::parseUrl(path, target_url) &&
                      webdav::parseUrl(root_opt->path, root_url)) {
                    std::string target_path = urlDecode(target_url.path);
                    std::string root_path = urlDecode(root_url.path);
                    while (root_path.size() > 1 && root_path.back() == '/') {
                      root_path.pop_back();
                    }
                    inside = target_url.https == root_url.https &&
                             target_url.host == root_url.host &&
                             target_url.port == root_url.port &&
                             target_path.rfind(root_path, 0) == 0;
                  }
                } else {
                  const fs::path canonical = fs::weakly_canonical(path, ec);
                  target = ec ? path : canonical.string();
                  const fs::path root_canonical =
                      fs::weakly_canonical(root_opt->path, ec);
                  root_canon =
                      ec ? root_opt->path : root_canonical.string();
                  inside = target.rfind(root_canon, 0) == 0;
                }
                if (!inside) {
                  result[i]["status"] = "error";
                  result[i]["error"] = "outside root";
                } else {
                  SQLite::Database &db =
                      db::conn(config::get().db_path.string());
                  const std::string vtarget =
                      storage::toVirtual(*root_opt, target);
                  const bool is_dir =
                      item.has("isDir") ? item["isDir"].b()
                                        : fs::is_directory(target, ec);
                  int removed = 0;
                  if (is_dir) {
                    SQLite::Statement q(
                        db, "DELETE FROM track WHERE root_id = ? AND (path = ? "
                            "OR (path >= ? AND path < ?))");
                    q.bind(1, root_id);
                    q.bind(2, vtarget);
                    q.bind(3, vtarget + "/");
                    q.bind(4, vtarget + "0");
                    q.executeStep();
                    removed = q.getChanges();
                  } else {
                    SQLite::Statement q(db,
                                        "DELETE FROM track WHERE path = ?");
                    q.bind(1, vtarget);
                    q.executeStep();
                    removed = q.getChanges();
                  }
                  db.exec("DELETE FROM album WHERE id NOT IN (SELECT "
                          "DISTINCT album_id FROM track WHERE album_id IS NOT "
                          "NULL)");
                  result[i]["status"] = "ok";
                  result[i]["tracks_removed"] = removed;
                }
              }
            } catch (...) {
              result[i]["status"] = "error";
              result[i]["error"] = "delete failed";
            }
          } else {
            result[i]["status"] = "error";
            result[i]["error"] = "unknown action";
          }
          ++i;
        }

        // One import task -> one job. All selected paths of a root are scanned
        // by a single job that runs the scan multi-threaded internally; jobs
        // themselves execute one at a time.
        for (auto &kv : scan_paths_by_root) {
          const int root_id = kv.first;
          const std::vector<std::string> paths = kv.second;
          const int job_id = scanner::enqueueJob("scan", [root_id, paths] {
            scanner::scanDirBatch(root_id, paths);
          });
          for (const int idx : scan_indices_by_root[root_id]) {
            result[idx]["status"] = "queued";
            result[idx]["jobId"] = job_id;
          }
        }
        if (!sync_from_ids.empty()) {
          const std::vector<int> ids = sync_from_ids;
          const int job_id = scanner::enqueueJob("sync-from-file", [ids] {
            runIdsParallel(ids, [](int id, int slot) {
              syncFromFileById(id, slot);
            });
          });
          for (const int idx : sync_from_indices) {
            result[idx]["status"] = "queued";
            result[idx]["jobId"] = job_id;
          }
        }
        if (!sync_to_ids.empty()) {
          const std::vector<int> ids = sync_to_ids;
          const int job_id = scanner::enqueueJob("sync-to-file", [ids] {
            runIdsParallel(ids, [](int id, int slot) {
              syncToFileById(id, slot);
            });
          });
          for (const int idx : sync_to_indices) {
            result[idx]["status"] = "queued";
            result[idx]["jobId"] = job_id;
          }
        }
        return jsonResponse(200, result.dump());
      });

  std::cout << "[boot] 生效端口 主=" << cfg.port << " Jellyfin="
            << (cfg.jellyfin_enabled ? std::to_string(cfg.jellyfin_port)
                                     : std::string("off"))
            << std::endl;

  std::thread jellyfin_thread;
  if (cfg.jellyfin_enabled) {
    jellyfin_thread = std::thread([&cfg]() { jellyfin::run(cfg); });
  }

  app.port(cfg.port).multithreaded().concurrency(8).run();

  if (jellyfin_thread.joinable()) {
    jellyfin_thread.join();
  }
  media_library::shutdown_library();
  image_cache::shutdown();
  return 0;
}
