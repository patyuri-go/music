#include "schema.h"

#include "schema_pg.h"



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

namespace {

bool isPostgres(db::Dialect dialect) {
  return dialect == db::Dialect::Postgres;
}

} // namespace

std::string insertOrIgnore(db::Dialect dialect, std::string_view table,
                           std::string_view columns, std::size_t param_count) {
  std::string out;
  out.reserve(table.size() + columns.size() + param_count * 2 + 48);
  if (isPostgres(dialect)) {
    out += "INSERT INTO ";
  } else {
    out += "INSERT OR IGNORE INTO ";
  }
  out += table;
  out += '(';
  out += columns;
  out += ") VALUES (";
  out += placeholders(param_count);
  out += ')';
  if (isPostgres(dialect)) {
    // 无冲突目标 = 任何唯一约束冲突都跳过，语义等同 INSERT OR IGNORE。
    out += " ON CONFLICT DO NOTHING";
  }
  return out;
}

std::string upsertLyrics(db::Dialect dialect) {
  if (isPostgres(dialect)) {
    return "INSERT INTO track_lyrics(track_id, text, synced) VALUES (?, ?, ?) "
           "ON CONFLICT(track_id) DO UPDATE SET text = EXCLUDED.text, "
           "synced = EXCLUDED.synced";
  }
  return "INSERT OR REPLACE INTO track_lyrics(track_id, text, synced) "
         "VALUES (?, ?, ?)";
}

std::string nowExpr(db::Dialect dialect) {
  if (isPostgres(dialect)) {
    // 与 SQLite 的 localtime 保持一致：写进库里的仍是本地时间文本。
    return "to_char(now(), 'YYYY-MM-DD HH24:MI:SS')";
  }
  return "datetime('now','localtime')";
}

std::string likeOp(db::Dialect dialect) {
  return isPostgres(dialect) ? "ILIKE" : "LIKE";
}

std::string artistIdsSubquery(db::Dialect dialect, std::string_view table,
                              std::string_view key_column,
                              std::string_view outer_key) {
  std::string out;
  out.reserve(160);
  out += "(SELECT ";
  if (isPostgres(dialect)) {
    out += "string_agg(artist_id::text, ',' ORDER BY position)";
  } else {
    out += "group_concat(artist_id, ',')";
  }
  out += " FROM ";
  if (isPostgres(dialect)) {
    out += table;
  } else {
    // SQLite 的 group_concat 不保证聚合顺序，先排序再聚合。
    out += "(SELECT artist_id, position FROM ";
    out += table;
    out += " WHERE ";
    out += key_column;
    out += " = ";
    out += outer_key;
    out += " ORDER BY position, artist_id)";
  }
  if (isPostgres(dialect)) {
    out += " WHERE ";
    out += key_column;
    out += " = ";
    out += outer_key;
  }
  out += ')';
  return out;
}

std::string artistInsert(db::Dialect dialect) {
  return insertOrIgnore(dialect, "artist", "name", 1);
}

std::string artistsSearchByName(db::Dialect dialect) {
  return "SELECT * FROM artist WHERE name " + likeOp(dialect) +
         " ? ORDER BY name, id LIMIT ?";
}

std::string albumById(db::Dialect dialect) {
  return "SELECT a.*, " +
         artistIdsSubquery(dialect, "album_artist", "album_id", "a.id") +
         " FROM album a WHERE a.id = ?";
}

std::string albumsByArtist(db::Dialect dialect) {
  return "SELECT a.*, " +
         artistIdsSubquery(dialect, "album_artist", "album_id", "a.id") +
         " FROM album a JOIN album_artist aa ON aa.album_id = a.id "
         "WHERE aa.artist_id = ? ORDER BY a.name";
}

std::string albumsSearchByName(db::Dialect dialect) {
  return "SELECT a.*, " +
         artistIdsSubquery(dialect, "album_artist", "album_id", "a.id") +
         " FROM album a WHERE a.name " + likeOp(dialect) +
         " ? ORDER BY a.name, a.id LIMIT ?";
}

std::string albumUpsert(db::Dialect dialect) {
  return insertOrIgnore(dialect, "album", "name, artist_key", 2);
}

std::string albumArtistInsert(db::Dialect dialect) {
  return insertOrIgnore(dialect, "album_artist",
                        "album_id, artist_id, position", 3);
}

std::string trackById(db::Dialect dialect) {
  return "SELECT t.*, " +
         artistIdsSubquery(dialect, "track_artist", "track_id", "t.id") +
         " FROM track t WHERE t.id = ?";
}

std::string tracksByAlbum(db::Dialect dialect) {
  return "SELECT t.*, " +
         artistIdsSubquery(dialect, "track_artist", "track_id", "t.id") +
         " FROM track t WHERE t.album_id = ? ORDER BY t.disc_no, t.track_no";
}

std::string tracksByArtist(db::Dialect dialect) {
  return "SELECT t.*, " +
         artistIdsSubquery(dialect, "track_artist", "track_id", "t.id") +
         " FROM track t JOIN track_artist ta ON ta.track_id = t.id "
         "WHERE ta.artist_id = ? ORDER BY t.added_at DESC LIMIT ?";
}

std::string trackInsert(db::Dialect dialect) {
  return "INSERT INTO track(root_id, path, title, album_id, disc_no, "
         "track_no, duration_ms, bitrate, sample_rate, channels, format, "
         "size_bytes, mtime, added_at, play_count, last_played, rating) "
         "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?," +
         nowExpr(dialect) + ",0,NULL,0)";
}

std::string trackByRootId(db::Dialect dialect) {
  return "SELECT t.*, " +
         artistIdsSubquery(dialect, "track_artist", "track_id", "t.id") +
         " FROM track t WHERE t.root_id = ?";
}

std::string trackUpdateStats(db::Dialect dialect) {
  return "UPDATE track SET play_count = play_count + 1, last_played = " +
         nowExpr(dialect) + " WHERE id = ?";
}

std::string tracksRecent(db::Dialect dialect) {
  return "SELECT t.*, " +
         artistIdsSubquery(dialect, "track_artist", "track_id", "t.id") +
         " FROM track t WHERE t.id > ? "
         "ORDER BY t.added_at DESC, t.id DESC LIMIT ?";
}

std::string tracksByGenre(db::Dialect dialect) {
  return "SELECT t.*, " +
         artistIdsSubquery(dialect, "track_artist", "track_id", "t.id") +
         " FROM track t JOIN track_genre tg ON t.id = tg.track_id "
         "WHERE tg.genre_id = ? ORDER BY t.added_at DESC LIMIT ?";
}

std::string searchByTitle(db::Dialect dialect) {
  return "SELECT t.*, " +
         artistIdsSubquery(dialect, "track_artist", "track_id", "t.id") +
         " FROM track t WHERE t.title " + likeOp(dialect) +
         " ? ORDER BY t.added_at DESC, t.id LIMIT ?";
}

std::string recentPlays(db::Dialect dialect) {
  return "SELECT t.*, " +
         artistIdsSubquery(dialect, "track_artist", "track_id", "t.id") +
         " FROM track t JOIN play_event pe ON t.id = pe.track_id "
         "ORDER BY pe.played_at DESC LIMIT ?";
}

std::string trackArtistInsert(db::Dialect dialect) {
  return insertOrIgnore(dialect, "track_artist",
                        "track_id, artist_id, position", 3);
}

std::string genreInsert(db::Dialect dialect) {
  return insertOrIgnore(dialect, "genre", "name", 1);
}

std::string trackGenreInsert(db::Dialect dialect) {
  return insertOrIgnore(dialect, "track_genre", "track_id, genre_id", 2);
}

std::string scanRootUpdateLastScan(db::Dialect dialect) {
  return "UPDATE scan_root SET last_scan = " + nowExpr(dialect) +
         " WHERE id = ?";
}

std::string trackLyricsUpsert(db::Dialect dialect) {
  return upsertLyrics(dialect);
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


std::vector<std::string> createStatements(db::Dialect dialect) {
  if (dialect == db::Dialect::Postgres) {
    return postgresCreateStatements();
  }
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
      "CREATE TABLE IF NOT EXISTS entity_link("
      "link_id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "entity_type TEXT NOT NULL "
      "CHECK(entity_type IN ('track','album','artist','album_artist','playlist')),"
      "created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),"
      "updated_at TEXT,"
      "UNIQUE(link_id, entity_type))",
      "CREATE TABLE IF NOT EXISTS entity_link_member("
      "member_id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "link_id INTEGER NOT NULL,"
      "library_id TEXT NOT NULL CHECK(length(library_id) > 0),"
      "entity_type TEXT NOT NULL "
      "CHECK(entity_type IN ('track','album','artist','album_artist','playlist')),"
      "entity_id TEXT NOT NULL CHECK(length(entity_id) > 0),"
      "confidence TEXT NOT NULL DEFAULT 'manual',"
      "verified INTEGER NOT NULL DEFAULT 1 CHECK(verified IN (0,1)),"
      "created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),"
      "updated_at TEXT,"
      "UNIQUE(library_id, entity_type, entity_id),"
      "UNIQUE(link_id, library_id),"
      "FOREIGN KEY(link_id, entity_type) "
      "REFERENCES entity_link(link_id, entity_type) ON DELETE CASCADE)",
      "CREATE TABLE IF NOT EXISTS entity_link_override("
      "link_id INTEGER NOT NULL,"
      "aspect TEXT NOT NULL "
      "CHECK(aspect IN ('metadata','lyrics','stream','artwork')),"
      "library_id TEXT NOT NULL,"
      "position INTEGER NOT NULL CHECK(position >= 0),"
      "created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),"
      "updated_at TEXT,"
      "PRIMARY KEY(link_id, aspect, library_id),"
      "UNIQUE(link_id, aspect, position),"
      "FOREIGN KEY(link_id, library_id) "
      "REFERENCES entity_link_member(link_id, library_id) ON DELETE CASCADE)",
      "CREATE INDEX IF NOT EXISTS idx_track_album ON track(album_id, disc_no, track_no)",
      "CREATE INDEX IF NOT EXISTS idx_track_added ON track(added_at)",
      "CREATE INDEX IF NOT EXISTS idx_track_root ON track(root_id)",
      "CREATE INDEX IF NOT EXISTS idx_track_artist_track ON track_artist(track_id)",
      "CREATE INDEX IF NOT EXISTS idx_track_artist_artist ON track_artist(artist_id)",
      "CREATE INDEX IF NOT EXISTS idx_album_artist_album ON album_artist(album_id)",
      "CREATE INDEX IF NOT EXISTS idx_album_artist_artist ON album_artist(artist_id)",
      "CREATE INDEX IF NOT EXISTS idx_play_event_time ON play_event(played_at)",
      "CREATE INDEX IF NOT EXISTS idx_entity_link_type ON entity_link(entity_type)",
  };
}

namespace {

std::int64_t cellInt(const db::Row &row, std::size_t index) {
  return std::get<std::int64_t>(row.at(index).value);
}

std::string cellText(const db::Row &row, std::size_t index) {
  return std::get<std::string>(row.at(index).value);
}

} // namespace

void migrate(db::Session &session) {
  for (const auto &statement : createStatements(db::Dialect::Sqlite)) {
    session.exec(statement);
  }

  bool hasVersion = false;
  int version = 0;
  {
    const db::ResultSet query = session.query(sql::kSchemaVersion);
    if (!query.rows.empty()) {
      hasVersion = true;
      version = static_cast<int>(cellInt(query.rows.front(), 0));
    }
  }

  if (!hasVersion) {
    session.exec("INSERT INTO schema_version(version) VALUES (10)");
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
    session.exec("DROP TABLE IF EXISTS imgtag");
    session.exec("DROP TABLE IF EXISTS tagid");
    session.exec("DROP TABLE IF EXISTS imginfor");
    session.exec("UPDATE schema_version SET version = 2");
    version = 2;
  }

  if (version == 2) {
    session.exec("PRAGMA foreign_keys = OFF;");
    try {
      session.exec("BEGIN");
      session.exec("DROP TABLE IF EXISTS album_new");
      session.exec(
          "CREATE TABLE album_new("
          "id INTEGER PRIMARY KEY,"
          "name TEXT NOT NULL,"
          "year INTEGER,"
          "cover_path TEXT,"
          "artist_key TEXT NOT NULL DEFAULT '',"
          "UNIQUE(name, artist_key))");
      session.exec(
          "INSERT OR IGNORE INTO album_new(id, name, year, cover_path, "
          "artist_key) SELECT al.id, al.name, al.year, al.cover_path, "
          "COALESCE((SELECT lower(ar.name) FROM artist ar "
          "WHERE ar.id = al.artist_id), '') FROM album al");
      session.exec(
          "INSERT OR IGNORE INTO album_artist(album_id, artist_id, position) "
          "SELECT id, artist_id, 0 FROM album WHERE artist_id IS NOT NULL");
      session.exec("DROP TABLE album");
      session.exec("ALTER TABLE album_new RENAME TO album");

      session.exec("DROP TABLE IF EXISTS track_new");
      session.exec(
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
      session.exec(
          "INSERT INTO track_new(id, root_id, path, title, album_id, disc_no, "
          "track_no, duration_ms, bitrate, sample_rate, channels, format, "
          "size_bytes, mtime, added_at, play_count, last_played, rating) "
          "SELECT id, root_id, path, title, album_id, disc_no, track_no, "
          "duration_ms, bitrate, sample_rate, channels, format, size_bytes, "
          "mtime, added_at, play_count, last_played, rating FROM track");
      session.exec(
          "INSERT OR IGNORE INTO track_artist(track_id, artist_id, position) "
          "SELECT id, artist_id, 0 FROM track WHERE artist_id IS NOT NULL");
      session.exec("DROP TABLE track");
      session.exec("ALTER TABLE track_new RENAME TO track");

      session.exec(
          "UPDATE track SET album_id = NULL WHERE album_id IS NOT NULL AND "
          "album_id NOT IN (SELECT id FROM album)");
      session.exec("UPDATE schema_version SET version = 3");
      version = 3;
      session.exec("COMMIT");
    } catch (...) {
      session.exec("ROLLBACK");
      session.exec("PRAGMA foreign_keys = ON;");
      throw;
    }
    session.exec("PRAGMA foreign_keys = ON;");
    for (const auto &statement : createStatements(db::Dialect::Sqlite)) {
      session.exec(statement);
    }
  }

  if (version == 3) {
    session.exec(
        "ALTER TABLE scan_root ADD COLUMN user TEXT NOT NULL DEFAULT ''");
    session.exec(
        "ALTER TABLE scan_root ADD COLUMN pass TEXT NOT NULL DEFAULT ''");
    session.exec("UPDATE schema_version SET version = 4");
    version = 4;
  }

  if (version == 4) {
    session.exec(
        "ALTER TABLE scan_root ADD COLUMN name TEXT NOT NULL DEFAULT ''");
    session.exec(
        "ALTER TABLE scan_root ADD COLUMN type TEXT NOT NULL DEFAULT ''");
    session.exec(
        "UPDATE scan_root SET type = CASE WHEN path LIKE 'http://%' OR "
        "path LIKE 'https://%' THEN 'webdav' ELSE 'local' END WHERE type = ''");
    session.exec("UPDATE schema_version SET version = 5");
    version = 5;
  }

  if (version == 5) {
    std::vector<repo::ScanRoot> roots;
    {
      const db::ResultSet query =
          session.query("SELECT id, path, name, type FROM scan_root");
      roots.reserve(query.rows.size());
      for (const db::Row &row : query.rows) {
        repo::ScanRoot r;
        r.id = static_cast<int>(cellInt(row, 0));
        r.path = cellText(row, 1);
        r.name = cellText(row, 2);
        r.type = cellText(row, 3);
        roots.push_back(std::move(r));
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
      session.execute("UPDATE scan_root SET name = ? WHERE id = ?",
                      {candidate, static_cast<std::int64_t>(r.id)});
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
    {
      const db::ResultSet query =
          session.query("SELECT id, root_id, path FROM track");
      for (const db::Row &row : query.rows) {
        const int tid = static_cast<int>(cellInt(row, 0));
        const int rid = static_cast<int>(cellInt(row, 1));
        const std::string real = cellText(row, 2);
        const auto root = rootById(rid);
        if (!root.has_value()) {
          continue;
        }
        const std::string v = storage::toVirtual(root->get(), real);
        if (v == real) {
          continue;
        }
        session.execute("UPDATE track SET path = ? WHERE id = ?",
                        {v, static_cast<std::int64_t>(tid)});
      }
    }
    std::vector<std::pair<int, std::string>> covers;
    {
      const db::ResultSet query = session.query(
          "SELECT id, cover_path FROM album WHERE cover_path <> ''");
      for (const db::Row &row : query.rows) {
        covers.emplace_back(static_cast<int>(cellInt(row, 0)), cellText(row, 1));
      }
    }
    for (auto &c : covers) {
      for (const auto &root : roots) {
        const std::string v = storage::toVirtual(root, c.second);
        if (v != c.second) {
          session.execute("UPDATE album SET cover_path = ? WHERE id = ?",
                          {v, static_cast<std::int64_t>(c.first)});
          break;
        }
      }
    }
    session.exec("UPDATE schema_version SET version = 6");
    version = 6;
  }

  if (version == 6) {
    session.exec(
        "CREATE TABLE IF NOT EXISTS track_lyrics("
        "track_id INTEGER PRIMARY KEY REFERENCES track(id) ON DELETE CASCADE,"
        "text TEXT NOT NULL DEFAULT '',"
        "synced INTEGER NOT NULL DEFAULT 0)");
    session.exec("UPDATE schema_version SET version = 7");
    version = 7;
  }

  if (version == 7) {
    // CREATE TABLE 已在 migrate 开始时执行；这里只推进版本。
    session.exec("UPDATE schema_version SET version = 8");
    version = 8;
  }

  if (version == 8) {
    // 库内映射表下线：对外 id 直接用本地库主键，远端用平台 id。
    session.exec("DROP INDEX IF EXISTS idx_entity_mapping_state");
    session.exec("DROP TABLE IF EXISTS entity_mapping");
    session.exec("UPDATE schema_version SET version = 9");
    version = 9;
  }

  if (version == 9) {
    // 三张链接表已由 createStatements() 创建；这里只推进版本。
    session.exec("UPDATE schema_version SET version = 10");
    return;
  }

  throw std::runtime_error("缺少从 schema 版本 " + std::to_string(version) +
                           " 升级到 " + std::to_string(kVersion) + " 的迁移步骤");
}

} // namespace schema
