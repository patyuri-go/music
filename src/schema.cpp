#include "schema.h"

#include "SQLiteCpp/Database.h"
#include "SQLiteCpp/Statement.h"

#include <sqlite3.h>

#include <set>
#include <stdexcept>
#include <string>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "storage.h"

namespace schema {
namespace sql {

std::string placeholders(std::size_t count) {
  std::string out;
  out.reserve(count * 2);
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) {
      out += ',';
    }
    out += '?';
  }
  return out;
}

} // namespace sql

namespace {

int hexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

std::string percentDecode(const std::string &s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hexValue(s[i + 1]);
      const int lo = hexValue(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

std::string deriveNameFromPath(const std::string &path) {
  std::string p = path;
  while (p.size() > 1 && p.back() == '/') {
    p.pop_back();
  }
  const auto slash = p.rfind('/');
  std::string seg = (slash == std::string::npos) ? p : p.substr(slash + 1);
  seg = percentDecode(seg);
  if (seg.empty()) {
    seg = "root";
  }
  return seg;
}

} // namespace

std::vector<std::string> createStatements() {
  return {
      "CREATE TABLE IF NOT EXISTS schema_version(version INTEGER NOT NULL)",
      "CREATE TABLE IF NOT EXISTS artist("
      "id INTEGER PRIMARY KEY,"
      "name TEXT NOT NULL UNIQUE,"
      "sort_name TEXT)",
      "CREATE TABLE IF NOT EXISTS album("
      "id INTEGER PRIMARY KEY,"
      "name TEXT NOT NULL,"
      "year INTEGER,"
      "cover_path TEXT,"
      "artist_key TEXT NOT NULL DEFAULT '',"
      "UNIQUE(name, artist_key))",
      "CREATE TABLE IF NOT EXISTS genre("
      "id INTEGER PRIMARY KEY,"
      "name TEXT NOT NULL UNIQUE)",
      "CREATE TABLE IF NOT EXISTS scan_root("
      "id INTEGER PRIMARY KEY,"
      "path TEXT NOT NULL UNIQUE,"
      "added_at TEXT DEFAULT (datetime('now','localtime')),"
      "last_scan TEXT,"
      "user TEXT NOT NULL DEFAULT '',"
      "pass TEXT NOT NULL DEFAULT '',"
      "name TEXT NOT NULL DEFAULT '',"
      "type TEXT NOT NULL DEFAULT '')",
      "CREATE TABLE IF NOT EXISTS track("
      "id INTEGER PRIMARY KEY,"
      "root_id INTEGER NOT NULL REFERENCES scan_root(id) ON DELETE CASCADE,"
      "path TEXT NOT NULL UNIQUE,"
      "title TEXT,"
      "album_id INTEGER REFERENCES album(id) ON DELETE SET NULL,"
      "disc_no INTEGER DEFAULT 1,"
      "track_no INTEGER,"
      "duration_ms INTEGER,"
      "bitrate INTEGER,"
      "sample_rate INTEGER,"
      "channels INTEGER,"
      "format TEXT,"
      "size_bytes INTEGER,"
      "mtime INTEGER,"
      "added_at TEXT DEFAULT (datetime('now','localtime')),"
      "play_count INTEGER NOT NULL DEFAULT 0,"
      "last_played TEXT,"
      "rating INTEGER DEFAULT 0)",
      "CREATE TABLE IF NOT EXISTS track_artist("
      "track_id INTEGER NOT NULL REFERENCES track(id) ON DELETE CASCADE,"
      "artist_id INTEGER NOT NULL REFERENCES artist(id) ON DELETE CASCADE,"
      "position INTEGER NOT NULL DEFAULT 0,"
      "PRIMARY KEY(track_id, artist_id))",
      "CREATE TABLE IF NOT EXISTS album_artist("
      "album_id INTEGER NOT NULL REFERENCES album(id) ON DELETE CASCADE,"
      "artist_id INTEGER NOT NULL REFERENCES artist(id) ON DELETE CASCADE,"
      "position INTEGER NOT NULL DEFAULT 0,"
      "PRIMARY KEY(album_id, artist_id))",
      "CREATE TABLE IF NOT EXISTS track_genre("
      "track_id INTEGER NOT NULL REFERENCES track(id) ON DELETE CASCADE,"
      "genre_id INTEGER NOT NULL REFERENCES genre(id) ON DELETE CASCADE,"
      "PRIMARY KEY(track_id, genre_id))",
      "CREATE TABLE IF NOT EXISTS track_lyrics("
      "track_id INTEGER PRIMARY KEY REFERENCES track(id) ON DELETE CASCADE,"
      "text TEXT NOT NULL DEFAULT '',"
      "synced INTEGER NOT NULL DEFAULT 0)",
      "CREATE TABLE IF NOT EXISTS playlist("
      "id INTEGER PRIMARY KEY,"
      "name TEXT NOT NULL,"
      "description TEXT,"
      "created_at TEXT DEFAULT (datetime('now','localtime')))",
      "CREATE TABLE IF NOT EXISTS playlist_track("
      "playlist_id INTEGER NOT NULL REFERENCES playlist(id) ON DELETE CASCADE,"
      "track_id INTEGER NOT NULL REFERENCES track(id) ON DELETE CASCADE,"
      "position INTEGER NOT NULL,"
      "PRIMARY KEY(playlist_id, position))",
      "CREATE TABLE IF NOT EXISTS play_event("
      "id INTEGER PRIMARY KEY,"
      "track_id INTEGER NOT NULL REFERENCES track(id) ON DELETE CASCADE,"
      "played_at TEXT DEFAULT (datetime('now','localtime')),"
      "seconds_played INTEGER)",
      "CREATE INDEX IF NOT EXISTS idx_track_album ON track(album_id, disc_no, track_no)",
      "CREATE INDEX IF NOT EXISTS idx_track_added ON track(added_at)",
      "CREATE INDEX IF NOT EXISTS idx_track_root ON track(root_id)",
      "CREATE INDEX IF NOT EXISTS idx_track_artist_track ON track_artist(track_id)",
      "CREATE INDEX IF NOT EXISTS idx_track_artist_artist ON track_artist(artist_id)",
      "CREATE INDEX IF NOT EXISTS idx_album_artist_album ON album_artist(album_id)",
      "CREATE INDEX IF NOT EXISTS idx_album_artist_artist ON album_artist(artist_id)",
      "CREATE INDEX IF NOT EXISTS idx_play_event_time ON play_event(played_at)",
      "CREATE TABLE IF NOT EXISTS entity_mapping("
      "library_id TEXT NOT NULL,"
      "entity_type TEXT NOT NULL,"
      "entity_id TEXT NOT NULL,"
      "internal_id INTEGER NOT NULL,"
      "state TEXT NOT NULL DEFAULT 'active',"
      "confidence REAL,"
      "created_at TEXT DEFAULT (datetime('now','localtime')),"
      "updated_at TEXT,"
      "PRIMARY KEY(library_id, entity_type, internal_id),"
      "UNIQUE(library_id, entity_type, entity_id))",
      "CREATE INDEX IF NOT EXISTS idx_entity_mapping_state ON "
      "entity_mapping(library_id, entity_type, state)",
  };
}

std::vector<StmtSpec> allStatements() {
  return {
      {"Artist::byName", sql::kArtistByName, 1},
      {"Artist::byId", sql::kArtistById, 1},
      {"Artist::insert", sql::kArtistInsert, 1},
      {"Album::byId", sql::kAlbumById, 1},
      {"Album::byArtist", sql::kAlbumsByArtist, 1},
      {"Album::upsert", sql::kAlbumUpsert, 2},
      {"Album::byKey", sql::kAlbumByKey, 2},
      {"Album::updateCover", sql::kAlbumUpdateCover, 2},
      {"Album::updateYear", sql::kAlbumUpdateYear, 2},
      {"Album::updateName", sql::kAlbumUpdateName, 2},
      {"Album::updateArtistKey", sql::kAlbumUpdateArtistKey, 2},
      {"Album::updateNameKey", sql::kAlbumUpdateNameKey, 3},
      {"Album::delete", sql::kAlbumDelete, 1},
      {"Album::reassignTracks", sql::kTrackReassignAlbum, 2},
      {"Album::artistClear", sql::kAlbumArtistClear, 1},
      {"Album::artistInsert", sql::kAlbumArtistInsert, 3},
      {"Album::artistsByAlbumId", sql::kAlbumArtistsByAlbumId, 1},
      {"Track::byId", sql::kTrackById, 1},
      {"Track::delete", sql::kTrackDelete, 1},
      {"Track::byAlbum", sql::kTracksByAlbum, 1},
      {"Track::byArtist", sql::kTracksByArtist, 2},
      {"Track::insert", sql::kTrackInsert, 13},
      {"Track::update", sql::kTrackUpdate, 12},
      {"Track::byPath", sql::kTrackByPath, 1},
      {"Track::byRootId", sql::kTrackByRootId, 1},
      {"Track::pathsUnderRoot", sql::kTrackPathsUnderRoot, 3},
      {"Track::deleteByPath", sql::kTrackDeleteByPath, 1},
      {"Track::genresByTrackId", sql::kTrackGenresByTrackId, 1},
      {"Track::updatePartial", sql::kTrackUpdatePartial, 13},
      {"Track::artistClear", sql::kTrackArtistClear, 1},
      {"Track::artistInsert", sql::kTrackArtistInsert, 3},
      {"Track::artistsByTrackId", sql::kTrackArtistsByTrackId, 1},
      {"Track::clearRating", sql::kTrackClearRating, 1},
      {"Track::updateStats", sql::kTrackUpdateStats, 1},
      {"Track::recent", sql::kTracksRecent, 2},
      {"Track::byGenre", sql::kTracksByGenre, 2},
      {"Track::search", sql::kSearchByTitle, 2},
      {"PlayEvent::recent", sql::kRecentPlays, 1},
      {"Genre::byName", sql::kGenreByName, 1},
      {"Genre::insert", sql::kGenreInsert, 1},
      {"Genre::trackInsert", sql::kTrackGenreInsert, 2},
      {"ScanRoot::all", sql::kScanRoots, 0},
      {"ScanRoot::insert", sql::kScanRootInsert, 5},
      {"ScanRoot::delete", sql::kScanRootDelete, 1},
      {"ScanRoot::byPath", sql::kScanRootByPath, 1},
      {"ScanRoot::byId", sql::kScanRootById, 1},
      {"ScanRoot::byName", sql::kScanRootByName, 1},
      {"ScanRoot::credentials", sql::kScanRootCredentials, 0},
      {"ScanRoot::updateLastScan", sql::kScanRootUpdateLastScan, 1},
      {"ScanRoot::updateName", sql::kScanRootUpdateName, 2},
      {"Playlist::all", sql::kPlaylists, 0},
      {"Playlist::tracks", sql::kPlaylistTracks, 1},
      {"TrackLyrics::upsert", sql::kTrackLyricsUpsert, 3},
      {"TrackLyrics::byTrackId", sql::kTrackLyricsByTrackId, 1},
      {"TrackLyrics::withLyrics", sql::kTracksWithLyrics, 0},
      {"schema::migrate", sql::kSchemaVersion, 0},
  };
}

void migrate(SQLite::Database &database) {
  for (const auto &statement : createStatements()) {
    database.exec(statement);
  }

  bool hasVersion = false;
  int version = 0;
  {
    SQLite::Statement query(database, sql::kSchemaVersion);
    if (query.executeStep()) {
      hasVersion = true;
      version = query.getColumn(0).getInt();
    }
  }

  if (!hasVersion) {
    database.exec("INSERT INTO schema_version(version) VALUES (8)");
    return;
  }

  if (version == kVersion) {
    return;
  }
  if (version > kVersion) {
    throw std::runtime_error("数据库 schema 版本为 " + std::to_string(version) +
                             "，高于本程序支持的 " + std::to_string(kVersion) +
                             "，拒绝启动以免损坏数据");
  }

  if (version == 1) {
    database.exec("DROP TABLE IF EXISTS imgtag");
    database.exec("DROP TABLE IF EXISTS tagid");
    database.exec("DROP TABLE IF EXISTS imginfor");
    database.exec("UPDATE schema_version SET version = 2");
    version = 2;
  }

  if (version == 2) {
    database.exec("PRAGMA foreign_keys = OFF;");
    try {
      database.exec("BEGIN");
      database.exec("DROP TABLE IF EXISTS album_new");
      database.exec(
          "CREATE TABLE album_new("
          "id INTEGER PRIMARY KEY,"
          "name TEXT NOT NULL,"
          "year INTEGER,"
          "cover_path TEXT,"
          "artist_key TEXT NOT NULL DEFAULT '',"
          "UNIQUE(name, artist_key))");
      database.exec(
          "INSERT OR IGNORE INTO album_new(id, name, year, cover_path, "
          "artist_key) SELECT al.id, al.name, al.year, al.cover_path, "
          "COALESCE((SELECT lower(ar.name) FROM artist ar "
          "WHERE ar.id = al.artist_id), '') FROM album al");
      database.exec(
          "INSERT OR IGNORE INTO album_artist(album_id, artist_id, position) "
          "SELECT id, artist_id, 0 FROM album WHERE artist_id IS NOT NULL");
      database.exec("DROP TABLE album");
      database.exec("ALTER TABLE album_new RENAME TO album");

      database.exec("DROP TABLE IF EXISTS track_new");
      database.exec(
          "CREATE TABLE track_new("
          "id INTEGER PRIMARY KEY,"
          "root_id INTEGER NOT NULL REFERENCES scan_root(id) ON DELETE CASCADE,"
          "path TEXT NOT NULL UNIQUE,"
          "title TEXT,"
          "album_id INTEGER REFERENCES album(id) ON DELETE SET NULL,"
          "disc_no INTEGER DEFAULT 1,"
          "track_no INTEGER,"
          "duration_ms INTEGER,"
          "bitrate INTEGER,"
          "sample_rate INTEGER,"
          "channels INTEGER,"
          "format TEXT,"
          "size_bytes INTEGER,"
          "mtime INTEGER,"
          "added_at TEXT DEFAULT (datetime('now','localtime')),"
          "play_count INTEGER NOT NULL DEFAULT 0,"
          "last_played TEXT,"
          "rating INTEGER DEFAULT 0)");
      database.exec(
          "INSERT INTO track_new(id, root_id, path, title, album_id, disc_no, "
          "track_no, duration_ms, bitrate, sample_rate, channels, format, "
          "size_bytes, mtime, added_at, play_count, last_played, rating) "
          "SELECT id, root_id, path, title, album_id, disc_no, track_no, "
          "duration_ms, bitrate, sample_rate, channels, format, size_bytes, "
          "mtime, added_at, play_count, last_played, rating FROM track");
      database.exec(
          "INSERT OR IGNORE INTO track_artist(track_id, artist_id, position) "
          "SELECT id, artist_id, 0 FROM track WHERE artist_id IS NOT NULL");
      database.exec("DROP TABLE track");
      database.exec("ALTER TABLE track_new RENAME TO track");

      database.exec(
          "UPDATE track SET album_id = NULL WHERE album_id IS NOT NULL AND "
          "album_id NOT IN (SELECT id FROM album)");
      database.exec("UPDATE schema_version SET version = 3");
      version = 3;
      database.exec("COMMIT");
    } catch (...) {
      database.exec("ROLLBACK");
      database.exec("PRAGMA foreign_keys = ON;");
      throw;
    }
    database.exec("PRAGMA foreign_keys = ON;");
    for (const auto &statement : createStatements()) {
      database.exec(statement);
    }
  }

  if (version == 3) {
    database.exec(
        "ALTER TABLE scan_root ADD COLUMN user TEXT NOT NULL DEFAULT ''");
    database.exec(
        "ALTER TABLE scan_root ADD COLUMN pass TEXT NOT NULL DEFAULT ''");
    database.exec("UPDATE schema_version SET version = 4");
    version = 4;
  }

  if (version == 4) {
    database.exec(
        "ALTER TABLE scan_root ADD COLUMN name TEXT NOT NULL DEFAULT ''");
    database.exec(
        "ALTER TABLE scan_root ADD COLUMN type TEXT NOT NULL DEFAULT ''");
    database.exec(
        "UPDATE scan_root SET type = CASE WHEN path LIKE 'http://%' OR "
        "path LIKE 'https://%' THEN 'webdav' ELSE 'local' END WHERE type = ''");
    database.exec("UPDATE schema_version SET version = 5");
    version = 5;
  }

  if (version == 5) {
    std::vector<repo::ScanRoot> roots;
    {
      SQLite::Statement q(database, "SELECT id, path, name, type FROM scan_root");
      while (q.executeStep()) {
        repo::ScanRoot r;
        r.id = q.getColumn(0).getInt();
        r.path = q.getColumn(1).getText();
        r.name = q.getColumn(2).getText();
        r.type = q.getColumn(3).getText();
        roots.push_back(r);
      }
    }
    std::set<std::string> used;
    for (const auto &r : roots) {
      if (!r.name.empty()) {
        used.insert(r.name);
      }
    }
    for (auto &r : roots) {
      if (!r.name.empty()) {
        continue;
      }
      std::string base = deriveNameFromPath(r.path);
      std::string candidate = base;
      int n = 2;
      while (used.count(candidate) != 0) {
        candidate = base + "-" + std::to_string(n++);
      }
      used.insert(candidate);
      r.name = candidate;
      SQLite::Statement u(database, "UPDATE scan_root SET name = ? WHERE id = ?");
      u.bind(1, candidate);
      u.bind(2, r.id);
      u.executeStep();
    }
    const auto rootById =
        [&roots](int id) -> std::optional<std::reference_wrapper<const repo::ScanRoot>> {
      for (const auto &r : roots) {
        if (r.id == id) {
          return std::cref(r);
        }
      }
      return std::nullopt;
    };
    std::vector<std::pair<int, std::string>> track_rows;
    {
      SQLite::Statement q(database, "SELECT id, root_id, path FROM track");
      while (q.executeStep()) {
        const int tid = q.getColumn(0).getInt();
        const int rid = q.getColumn(1).getInt();
        const std::string real = q.getColumn(2).getText();
        const auto root = rootById(rid);
        if (!root.has_value()) {
          continue;
        }
        const std::string v = storage::toVirtual(root->get(), real);
        if (v == real) {
          continue;
        }
        SQLite::Statement u(database, "UPDATE track SET path = ? WHERE id = ?");
        u.bind(1, v);
        u.bind(2, tid);
        u.executeStep();
      }
    }
    std::vector<std::pair<int, std::string>> covers;
    {
      SQLite::Statement q(database,
                          "SELECT id, cover_path FROM album WHERE cover_path <> ''");
      while (q.executeStep()) {
        covers.emplace_back(q.getColumn(0).getInt(), q.getColumn(1).getText());
      }
    }
    for (auto &c : covers) {
      for (const auto &root : roots) {
        const std::string v = storage::toVirtual(root, c.second);
        if (v != c.second) {
          SQLite::Statement u(database,
                              "UPDATE album SET cover_path = ? WHERE id = ?");
          u.bind(1, v);
          u.bind(2, c.first);
          u.executeStep();
          break;
        }
      }
    }
    database.exec("UPDATE schema_version SET version = 6");
    version = 6;
  }

  if (version == 6) {
    database.exec(
        "CREATE TABLE IF NOT EXISTS track_lyrics("
        "track_id INTEGER PRIMARY KEY REFERENCES track(id) ON DELETE CASCADE,"
        "text TEXT NOT NULL DEFAULT '',"
        "synced INTEGER NOT NULL DEFAULT 0)");
    database.exec("UPDATE schema_version SET version = 7");
    version = 7;
  }

  if (version == 7) {
    // CREATE TABLE 已在 migrate 开始时执行；这里只推进版本。
    database.exec("UPDATE schema_version SET version = 8");
    version = 8;
    return;
  }

  throw std::runtime_error("缺少从 schema 版本 " + std::to_string(version) +
                           " 升级到 " + std::to_string(kVersion) + " 的迁移步骤");
}

void selfCheck(SQLite::Database &database) {
  sqlite3 *handle = database.getHandle();

  for (const auto &spec : allStatements()) {
    sqlite3_stmt *raw = nullptr;
    const char *tail = nullptr;
    const int rc =
        sqlite3_prepare_v2(handle, spec.sql.c_str(), -1, &raw, &tail);
    // prepared 持有语句生命周期，避免手写 finalize。
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> prepared(
        raw, &sqlite3_finalize);

    if (rc != SQLITE_OK || !prepared) {
      throw std::runtime_error(
          "启动自检失败：语句 " + spec.name + " 无法 prepare（" +
          std::string(sqlite3_errmsg(handle)) + "）\n  SQL: " + spec.sql);
    }

    const int actual = sqlite3_bind_parameter_count(prepared.get());
    const bool fullyConsumed = (tail != nullptr && *tail == '\0');

    if (!fullyConsumed) {
      throw std::runtime_error("启动自检失败：语句 " + spec.name +
                               " 含有除首条之外的多余内容\n  SQL: " + spec.sql);
    }

    if (actual != spec.params) {
      throw std::runtime_error(
          "启动自检失败：语句 " + spec.name + " 的绑定参数个数不符（登记期望 " +
          std::to_string(spec.params) + "，SQL 中实际 " + std::to_string(actual) +
          "）—— 通常是代码里拼了占位符却忘了 bind\n  SQL: " + spec.sql);
    }
  }
}

} // namespace schema
