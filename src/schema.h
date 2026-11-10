#ifndef SCHEMA_H
#define SCHEMA_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "db/session.h"

namespace schema {

/// 当前 schema 版本（v10：新增跨库实体链接三张表）。
inline constexpr int kVersion = 10;

/// 自检时生成 IN 列表语句的最大长度（P1 暂未使用，保留）。
inline constexpr std::size_t kMaxInList = 8;

/// 建表语句的唯一事实来源：DDL 只写在这里，其它地方一律不得再写 CREATE。
///
/// SQLite 分支逐字返回历史 DDL（列顺序、类型名、DEFAULT 文本都与旧版一致），
/// 保证已有 music.db 不受影响；PostgreSQL 分支返回语义等价、但用 PG 类型与
/// 表达式的 DDL。两条分支的语句数量与顺序一一对应。
std::vector<std::string> createStatements(db::Dialect dialect);

/// PostgreSQL 空库建库：执行 createStatements(Postgres)，再校验 schema_version。
///
/// PostgreSQL 不提供从旧版本升级的迁移路径（本项目只有 SQLite 有历史数据）：
/// 无版本行则写入当前版本；版本低于 kVersion 或高于 kVersion 都拒绝启动。
void ensurePostgresSchema(db::Session &session);

/// 运行期 SQL 的唯一事实来源。
///
/// 与方言无关的语句保留为 `inline constexpr const char *`；随方言变化的语句
/// 一律写成 `函数(db::Dialect)`。函数里占位符**统一写 `?`**，由 SociSession
/// 在 PostgreSQL 分支改写成 `$1..$n`，因此调用点的参数顺序与 SQLite 完全一致。
namespace sql {

std::string placeholders(std::size_t count);

/// PostgreSQL 没有 `INSERT OR IGNORE`，改写成 `ON CONFLICT DO NOTHING`。
/// `table` 形如 `"artist"`，`columns` 形如 `"name, artist_key"`，
/// `param_count` 决定 `?` 的个数。
std::string insertOrIgnore(db::Dialect dialect, std::string_view table,
                           std::string_view columns, std::size_t param_count);

/// 歌词写入：SQLite 用 `INSERT OR REPLACE`，PostgreSQL 用
/// `ON CONFLICT(track_id) DO UPDATE`。固定 3 个参数。
std::string upsertLyrics(db::Dialect dialect);

/// 时间表达式：SQLite `datetime('now','localtime')`，
/// PostgreSQL `to_char(now(), 'YYYY-MM-DD HH24:MI:SS')`。
/// 两边都产出「本地时间 YYYY-MM-DD HH:MM:SS」文本，语义一致。
std::string nowExpr(db::Dialect dialect);

/// 忽略 ASCII 大小写的包含匹配运算符：SQLite `LIKE`，PostgreSQL `ILIKE`。
std::string likeOp(db::Dialect dialect);

/// 「同一 key 的全部 artist_id 拼成逗号串」的标量子查询，按 position 排序。
/// `table` 形如 `"track_artist"`，`key_column` 形如 `"track_id"`，
/// `outer_key` 是外层表里的关联列（例如 `"t.id"`）。
///
/// SQLite 的 group_concat 不能可靠地按行序聚合，因此套一层带 ORDER BY 的子查询；
/// PostgreSQL 用 `string_agg(... ORDER BY position)`。旧版 SQL 缺 ORDER BY，
/// 导致 artist_ids 顺序不稳定，这里补上。
std::string artistIdsSubquery(db::Dialect dialect, std::string_view table,
                              std::string_view key_column,
                              std::string_view outer_key);

// artist
inline constexpr const char *kArtistByName =
    "SELECT id FROM artist WHERE name = ?";
inline constexpr const char *kArtistById = "SELECT * FROM artist WHERE id = ?";
std::string artistInsert(db::Dialect dialect);
// 搜索：艺术家名包含匹配（由媒体库层做归一，SQL 只负责粗筛）。
std::string artistsSearchByName(db::Dialect dialect);

// album
// 注意：album 不再有单列 artist_id；艺术家一律经 album_artist 多对多。
// artist_key 是“去重标识键”（由专辑艺术家名排序拼接而成），不是某个艺术家。
std::string albumById(db::Dialect dialect);
std::string albumsByArtist(db::Dialect dialect);
std::string albumsSearchByName(db::Dialect dialect);
std::string albumUpsert(db::Dialect dialect);
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
std::string albumArtistInsert(db::Dialect dialect);
inline constexpr const char *kAlbumArtistsByAlbumId =
    "SELECT artist_id FROM album_artist WHERE album_id = ? ORDER BY position, artist_id";

// track
// 注意：track 不再有单列 artist_id；艺术家一律经 track_artist 多对多。
// 下列查询在 t.* 之后追加一列：按 position 排序的 artist_id 逗号串。
// 追加列的表达式请用 artistIdsSubquery(dialect, "track_artist", "track_id", "t.id")。
inline constexpr const char *kTrackDelete = "DELETE FROM track WHERE id = ?";
std::string trackById(db::Dialect dialect);
std::string tracksByAlbum(db::Dialect dialect);
std::string tracksByArtist(db::Dialect dialect);
std::string trackInsert(db::Dialect dialect);
inline constexpr const char *kTrackUpdate =
    "UPDATE track SET title = ?, album_id = ?, disc_no = ?, "
    "track_no = ?, duration_ms = ?, bitrate = ?, sample_rate = ?, "
    "channels = ?, format = ?, size_bytes = ?, mtime = ? WHERE path = ?";
inline constexpr const char *kTrackByPath =
    "SELECT id, mtime, size_bytes, album_id FROM track WHERE path = ?";
std::string trackByRootId(db::Dialect dialect);
inline constexpr const char *kTrackPathsUnderRoot =
    "SELECT id, path, title FROM track "
    "WHERE root_id = ? AND path >= ? AND path < ?";
inline constexpr const char *kTrackClearRating =
    "UPDATE track SET rating = 0 WHERE id = ?";
std::string trackUpdateStats(db::Dialect dialect);
inline constexpr const char *kTrackDeleteByPath =
    "DELETE FROM track WHERE path = ?";
inline constexpr const char *kTrackGenresByTrackId =
    "SELECT genre_id FROM track_genre WHERE track_id = ?";
inline constexpr const char *kTrackUpdatePartial =
    "UPDATE track SET title = ?, album_id = ?, "
    "disc_no = ?, track_no = ?, duration_ms = ?, bitrate = ?, "
    "sample_rate = ?, channels = ?, format = ?, size_bytes = ?, "
    "mtime = ?, rating = ? WHERE id = ?";
std::string tracksRecent(db::Dialect dialect);
std::string tracksByGenre(db::Dialect dialect);
std::string searchByTitle(db::Dialect dialect);
std::string recentPlays(db::Dialect dialect);

// track_artist (多对多)
inline constexpr const char *kTrackArtistClear =
    "DELETE FROM track_artist WHERE track_id = ?";
std::string trackArtistInsert(db::Dialect dialect);
inline constexpr const char *kTrackArtistsByTrackId =
    "SELECT artist_id FROM track_artist WHERE track_id = ? ORDER BY position, artist_id";

// genre
inline constexpr const char *kGenreByName =
    "SELECT id FROM genre WHERE name = ?";
std::string genreInsert(db::Dialect dialect);
std::string trackGenreInsert(db::Dialect dialect);

// scan_root
// `user` 在 PostgreSQL 里是保留字，因此这里统一写成带双引号的标识符；
// SQLite 同样接受双引号标识符，两边共用一份 SQL。
inline constexpr const char *kScanRoots = "SELECT * FROM scan_root";
inline constexpr const char *kScanRootInsert =
    "INSERT INTO scan_root(path, \"user\", pass, name, type) "
    "VALUES (?, ?, ?, ?, ?)";
inline constexpr const char *kScanRootDelete =
    "DELETE FROM scan_root WHERE id = ?";
inline constexpr const char *kScanRootByPath =
    "SELECT id FROM scan_root WHERE path = ?";
inline constexpr const char *kScanRootById =
    "SELECT * FROM scan_root WHERE id = ?";
inline constexpr const char *kScanRootByName =
    "SELECT * FROM scan_root WHERE name = ?";
inline constexpr const char *kScanRootCredentials =
    "SELECT path, \"user\", pass FROM scan_root WHERE \"user\" <> ''";
std::string scanRootUpdateLastScan(db::Dialect dialect);
inline constexpr const char *kScanRootUpdateName =
    "UPDATE scan_root SET name = ? WHERE id = ?";

// track_lyrics
std::string trackLyricsUpsert(db::Dialect dialect);
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

/// 建库 / 迁移到 kVersion（仅 SQLite 有历史数据，只对 SQLite 实现迁移）。
void migrate(db::Session &session);

} // namespace schema

#endif
