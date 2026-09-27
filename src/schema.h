#ifndef SCHEMA_H
#define SCHEMA_H

#include <cstddef>
#include <string>
#include <vector>

namespace SQLite {
class Database;
} // namespace SQLite

namespace schema {

/// 当前 schema 版本（v4：scan_root 增加 user/pass 凭据）。
inline constexpr int kVersion = 7;

/// 自检时生成 IN 列表语句的最大长度（P1 暂未使用，保留）。
inline constexpr std::size_t kMaxInList = 8;

/// 建表语句的唯一事实来源：DDL 只写在这里，其它地方一律不得再写 CREATE。
std::vector<std::string> createStatements();

/// 运行期 SQL 的唯一事实来源。
namespace sql {

std::string placeholders(std::size_t count);

// artist
inline constexpr const char *kArtistByName =
    "SELECT id FROM artist WHERE name = ?";
inline constexpr const char *kArtistById = "SELECT * FROM artist WHERE id = ?";
inline constexpr const char *kArtistInsert =
    "INSERT OR IGNORE INTO artist(name) VALUES (?)";

// album
// 注意：album 不再有单列 artist_id；艺术家一律经 album_artist 多对多。
// artist_key 是“去重标识键”（由专辑艺术家名排序拼接而成），不是某个艺术家。
inline constexpr const char *kAlbumById =
    "SELECT a.*, (SELECT group_concat(artist_id, ',') FROM album_artist "
    "WHERE album_id = a.id) FROM album a WHERE a.id = ?";
inline constexpr const char *kAlbumsByArtist =
    "SELECT a.*, (SELECT group_concat(artist_id, ',') FROM album_artist "
    "WHERE album_id = a.id) FROM album a JOIN album_artist aa ON "
    "aa.album_id = a.id WHERE aa.artist_id = ? ORDER BY a.name";
inline constexpr const char *kAlbumUpsert =
    "INSERT OR IGNORE INTO album(name, artist_key) VALUES (?, ?)";
inline constexpr const char *kAlbumByKey =
    "SELECT id FROM album WHERE name = ? AND artist_key = ?";
inline constexpr const char *kAlbumUpdateCover =
    "UPDATE album SET cover_path = ? WHERE id = ?";
inline constexpr const char *kAlbumUpdateYear =
    "UPDATE album SET year = ? WHERE id = ?";
inline constexpr const char *kAlbumUpdateName =
    "UPDATE album SET name = ? WHERE id = ?";
inline constexpr const char *kAlbumUpdateArtistKey =
    "UPDATE album SET artist_key = ? WHERE id = ?";
inline constexpr const char *kAlbumUpdateNameKey =
    "UPDATE album SET name = ?, artist_key = ? WHERE id = ?";
inline constexpr const char *kAlbumDelete = "DELETE FROM album WHERE id = ?";
inline constexpr const char *kTrackReassignAlbum =
    "UPDATE track SET album_id = ? WHERE album_id = ?";

// album_artist (多对多)
inline constexpr const char *kAlbumArtistClear =
    "DELETE FROM album_artist WHERE album_id = ?";
inline constexpr const char *kAlbumArtistInsert =
    "INSERT OR IGNORE INTO album_artist(album_id, artist_id, position) "
    "VALUES (?, ?, ?)";
inline constexpr const char *kAlbumArtistsByAlbumId =
    "SELECT artist_id FROM album_artist WHERE album_id = ? ORDER BY position, artist_id";

// track
// 注意：track 不再有单列 artist_id；艺术家一律经 track_artist 多对多。
// 下列查询在 t.* 之后追加一列：按 position 排序的 artist_id 逗号串。
inline constexpr const char *kTrackArtistsExpr =
    "(SELECT group_concat(artist_id, ',') FROM track_artist WHERE track_id = t.id)";
inline constexpr const char *kTrackById =
    "SELECT t.*, (SELECT group_concat(artist_id, ',') FROM track_artist "
    "WHERE track_id = t.id) FROM track t WHERE t.id = ?";
inline constexpr const char *kTrackDelete = "DELETE FROM track WHERE id = ?";
inline constexpr const char *kTracksByAlbum =
    "SELECT t.*, (SELECT group_concat(artist_id, ',') FROM track_artist "
    "WHERE track_id = t.id) FROM track t WHERE t.album_id = ? "
    "ORDER BY t.disc_no, t.track_no";
inline constexpr const char *kTracksByArtist =
    "SELECT t.*, (SELECT group_concat(artist_id, ',') FROM track_artist "
    "WHERE track_id = t.id) FROM track t JOIN track_artist ta ON "
    "ta.track_id = t.id WHERE ta.artist_id = ? ORDER BY t.added_at DESC LIMIT ?";
inline constexpr const char *kTrackInsert =
    "INSERT INTO track(root_id, path, title, album_id, disc_no, "
    "track_no, duration_ms, bitrate, sample_rate, channels, format, "
    "size_bytes, mtime, added_at, play_count, last_played, rating) "
    "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,datetime('now','localtime'),0,NULL,0)";
inline constexpr const char *kTrackUpdate =
    "UPDATE track SET title = ?, album_id = ?, disc_no = ?, "
    "track_no = ?, duration_ms = ?, bitrate = ?, sample_rate = ?, "
    "channels = ?, format = ?, size_bytes = ?, mtime = ? WHERE path = ?";
inline constexpr const char *kTrackByPath =
    "SELECT id, mtime, size_bytes, album_id FROM track WHERE path = ?";
inline constexpr const char *kTrackByRootId =
    "SELECT t.*, (SELECT group_concat(artist_id, ',') FROM track_artist "
    "WHERE track_id = t.id) FROM track t WHERE t.root_id = ?";
inline constexpr const char *kTrackPathsUnderRoot =
    "SELECT id, path, title FROM track "
    "WHERE root_id = ? AND path >= ? AND path < ?";
inline constexpr const char *kTrackClearRating =
    "UPDATE track SET rating = 0 WHERE id = ?";
inline constexpr const char *kTrackUpdateStats =
    "UPDATE track SET play_count = play_count + 1, last_played = datetime('now','localtime') WHERE id = ?";
inline constexpr const char *kTrackDeleteByPath =
    "DELETE FROM track WHERE path = ?";
inline constexpr const char *kTrackGenresByTrackId =
    "SELECT genre_id FROM track_genre WHERE track_id = ?";
inline constexpr const char *kTrackUpdatePartial =
    "UPDATE track SET title = ?, album_id = ?, "
    "disc_no = ?, track_no = ?, duration_ms = ?, bitrate = ?, "
    "sample_rate = ?, channels = ?, format = ?, size_bytes = ?, "
    "mtime = ?, rating = ? WHERE id = ?";
inline constexpr const char *kTracksRecent =
    "SELECT t.*, (SELECT group_concat(artist_id, ',') FROM track_artist "
    "WHERE track_id = t.id) FROM track t WHERE t.id > ? "
    "ORDER BY t.added_at DESC, t.id DESC LIMIT ?";
inline constexpr const char *kTracksByGenre =
    "SELECT t.*, (SELECT group_concat(artist_id, ',') FROM track_artist "
    "WHERE track_id = t.id) FROM track t JOIN track_genre tg ON "
    "t.id = tg.track_id WHERE tg.genre_id = ? ORDER BY t.added_at DESC LIMIT ?";
inline constexpr const char *kSearchByTitle =
    "SELECT t.*, (SELECT group_concat(artist_id, ',') FROM track_artist "
    "WHERE track_id = t.id) FROM track t WHERE t.title LIKE ? "
    "ORDER BY t.added_at DESC LIMIT ?";
inline constexpr const char *kRecentPlays =
    "SELECT t.*, (SELECT group_concat(artist_id, ',') FROM track_artist "
    "WHERE track_id = t.id) FROM track t JOIN play_event pe ON "
    "t.id = pe.track_id ORDER BY pe.played_at DESC LIMIT ?";

// track_artist (多对多)
inline constexpr const char *kTrackArtistClear =
    "DELETE FROM track_artist WHERE track_id = ?";
inline constexpr const char *kTrackArtistInsert =
    "INSERT OR IGNORE INTO track_artist(track_id, artist_id, position) "
    "VALUES (?, ?, ?)";
inline constexpr const char *kTrackArtistsByTrackId =
    "SELECT artist_id FROM track_artist WHERE track_id = ? ORDER BY position, artist_id";

// genre
inline constexpr const char *kGenreByName =
    "SELECT id FROM genre WHERE name = ?";
inline constexpr const char *kGenreInsert =
    "INSERT OR IGNORE INTO genre(name) VALUES (?)";
inline constexpr const char *kTrackGenreInsert =
    "INSERT OR IGNORE INTO track_genre(track_id, genre_id) VALUES (?, ?)";

// scan_root
inline constexpr const char *kScanRoots = "SELECT * FROM scan_root";
inline constexpr const char *kScanRootInsert =
    "INSERT INTO scan_root(path, user, pass, name, type) VALUES (?, ?, ?, ?, ?)";
inline constexpr const char *kScanRootDelete =
    "DELETE FROM scan_root WHERE id = ?";
inline constexpr const char *kScanRootByPath =
    "SELECT id FROM scan_root WHERE path = ?";
inline constexpr const char *kScanRootById =
    "SELECT * FROM scan_root WHERE id = ?";
inline constexpr const char *kScanRootByName =
    "SELECT * FROM scan_root WHERE name = ?";
inline constexpr const char *kScanRootCredentials =
    "SELECT path, user, pass FROM scan_root WHERE user <> ''";
inline constexpr const char *kScanRootUpdateLastScan =
    "UPDATE scan_root SET last_scan = datetime('now','localtime') WHERE id = ?";
inline constexpr const char *kScanRootUpdateName =
    "UPDATE scan_root SET name = ? WHERE id = ?";

// track_lyrics
inline constexpr const char *kTrackLyricsUpsert =
    "INSERT OR REPLACE INTO track_lyrics(track_id, text, synced) VALUES (?, ?, ?)";
inline constexpr const char *kTrackLyricsByTrackId =
    "SELECT text, synced FROM track_lyrics WHERE track_id = ?";
inline constexpr const char *kTracksWithLyrics =
    "SELECT track_id FROM track_lyrics WHERE text <> ''";

// playlist
inline constexpr const char *kPlaylists = "SELECT * FROM playlist";
inline constexpr const char *kPlaylistTracks =
    "SELECT t.* FROM track t JOIN playlist_track pt ON t.id = pt.track_id "
    "WHERE pt.playlist_id = ? ORDER BY pt.position";

inline constexpr const char *kSchemaVersion =
    "SELECT version FROM schema_version LIMIT 1";

} // namespace sql

/// 一条需要在启动时自检的语句：名称、SQL 文本、期望绑定的参数个数。
struct StmtSpec {
  std::string name;
  std::string sql;
  int params;
};

/// 需要自检的全部语句。
std::vector<StmtSpec> allStatements();

/// 建库 / 迁移到 kVersion。
void migrate(SQLite::Database &database);

/// 启动自检：逐条 prepare，并校验 SQL 中的绑定参数个数与登记值一致。
void selfCheck(SQLite::Database &database);

} // namespace schema

#endif
