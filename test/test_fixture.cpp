// 集成测试夹具：直接向中央库写入固定数据，或按前缀清理。
//
//   ./test_fixture seed    [--config /path/to/config.json]
//   ./test_fixture cleanup [--config /path/to/config.json]
//
// seed 成功后向 stdout 输出 shell 可 eval 的 KEY='VALUE' 行；
// 测试脚本据此拿到真实行 id。所有数据都带 __fixture__ 前缀，可重复执行。

#include "config.h"
#include "db.h"
#include "schema.h"

#include "fixture_ids.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using db::ParamList;
using db::Value;

std::int64_t scalarId(const db::ResultSet &result) {
  if (result.rows.empty() || result.rows.front().empty()) {
    return 0;
  }
  const db::Cell &cell = result.rows.front().front();
  if (cell.is_null()) {
    return 0;
  }
  if (const auto *value = std::get_if<std::int64_t>(&cell.value)) {
    return *value;
  }
  if (const auto *value = std::get_if<std::string>(&cell.value)) {
    return std::stoll(*value);
  }
  return 0;
}

std::int64_t queryId(db::Session &session, const std::string &sql,
                     const ParamList &params) {
  return scalarId(session.query(sql, params));
}

void cleanup(db::Session &session) {
  // 先删歌单（playlist_track 级联）；再删扫描根（track 及其从属表级联）；
  // 最后删专辑与艺术家。全部按 __fixture__ 前缀限定。
  session.execute("DELETE FROM playlist WHERE name LIKE ?",
                  {std::string(fixture::kPrefix) + "%"});
  session.execute("DELETE FROM scan_root WHERE path = ?",
                  {fixture::kRootPath});
  session.execute("DELETE FROM album WHERE name LIKE ?",
                  {std::string(fixture::kPrefix) + "%"});
  session.execute("DELETE FROM artist WHERE name LIKE ?",
                  {std::string(fixture::kPrefix) + "%"});
}

std::int64_t ensureArtist(db::Session &session, const std::string &name) {
  session.execute(schema::sql::artistInsert(session.dialect()),
                  {name});
  return queryId(session, schema::sql::kArtistByName, {name});
}

std::int64_t insertAlbum(db::Session &session, const std::string &name,
                         const std::string &artist_key,
                         const std::string &cover_path,
                         const std::string &entity_id) {
  Value entity = entity_id.empty() ? Value(db::Null{}) : Value(entity_id);
  Value cover = cover_path.empty() ? Value(db::Null{}) : Value(cover_path);
  session.execute(
      "INSERT INTO album(name, artist_key, year, cover_path, entity_id) "
      "VALUES (?, ?, ?, ?, ?)",
      {name, artist_key, static_cast<std::int64_t>(2024), cover, entity});
  return queryId(session, schema::sql::kAlbumByKey, {name, artist_key});
}

std::int64_t insertTrack(db::Session &session, std::int64_t root_id,
                         const std::string &path, const std::string &title,
                         std::int64_t album_id, std::int64_t track_no,
                         const std::string &entity_id) {
  Value entity = entity_id.empty() ? Value(db::Null{}) : Value(entity_id);
  const std::string sql =
      "INSERT INTO track(root_id, path, title, album_id, disc_no, track_no, "
      "duration_ms, bitrate, sample_rate, channels, format, size_bytes, mtime, "
      "added_at, play_count, last_played, rating, entity_id) "
      "VALUES (?, ?, ?, ?, 1, ?, ?, 1411200, 44100, 2, ?, ?, 0, " +
      schema::sql::nowExpr(session.dialect()) + ", 0, NULL, 0, ?)";
  session.execute(sql,
                  {root_id, path, title, album_id,
                   static_cast<std::int64_t>(track_no), fixture::kDurationMs,
                   fixture::kFormat, fixture::kSizeBytes, entity});
  return queryId(session, schema::sql::kTrackByPath, {path});
}

void linkTrackArtist(db::Session &session, std::int64_t track_id,
                     std::int64_t artist_id, std::int64_t position) {
  session.execute(schema::sql::trackArtistInsert(session.dialect()),
                  {track_id, artist_id, position});
}

void linkAlbumArtist(db::Session &session, std::int64_t album_id,
                     std::int64_t artist_id, std::int64_t position) {
  session.execute(schema::sql::albumArtistInsert(session.dialect()),
                  {album_id, artist_id, position});
}

void setLyrics(db::Session &session, std::int64_t track_id,
               const std::string &text, bool synced) {
  session.execute(schema::sql::trackLyricsUpsert(session.dialect()),
                  {track_id, text, static_cast<std::int64_t>(synced ? 1 : 0)});
}

void printEnv(const char *key, const std::string &value) {
  std::cout << key << "='" << value << "'\n";
}

void printEnv(const char *key, std::int64_t value) {
  std::cout << key << "='" << value << "'\n";
}

void seed(db::Session &session) {
  cleanup(session);

  session.execute(schema::sql::kScanRootInsert,
                  {fixture::kRootPath, std::string(), std::string(),
                   fixture::kPrefix + "root", std::string("local")});
  const std::int64_t root_id =
      queryId(session, schema::sql::kScanRootByPath, {fixture::kRootPath});

  const std::int64_t album_id =
      insertAlbum(session, fixture::kAlbumName, fixture::kAlbumArtistKey,
                  fixture::kAlbumCoverPath, fixture::kAlbumEntity);
  const std::int64_t album_null_id =
      insertAlbum(session, fixture::kAlbumNullName,
                  fixture::kAlbumNullArtistKey, std::string(),
                  fixture::kAlbumNullEntity);

  // 角色串里的 4 个名字（原先由扫描器解析，现在直接建关系）。
  std::vector<std::int64_t> role_artist_ids;
  for (const auto &name : fixture::kRoleArtists) {
    role_artist_ids.push_back(ensureArtist(session, name));
  }
  std::vector<std::int64_t> plain_artist_ids;
  for (const auto &name : fixture::kPlainArtists) {
    plain_artist_ids.push_back(ensureArtist(session, name));
  }
  for (std::size_t i = 0; i < role_artist_ids.size(); ++i) {
    linkAlbumArtist(session, album_id, role_artist_ids[i],
                    static_cast<std::int64_t>(i));
  }

  const std::int64_t track_role =
      insertTrack(session, root_id, fixture::kTrackRolePath,
                  fixture::kTrackRoleTitle, album_id, 1,
                  fixture::kTrackRoleEntity);
  for (std::size_t i = 0; i < role_artist_ids.size(); ++i) {
    linkTrackArtist(session, track_role, role_artist_ids[i],
                    static_cast<std::int64_t>(i));
  }
  setLyrics(session, track_role, fixture::kSyncedLyrics, true);

  const std::int64_t track_plain =
      insertTrack(session, root_id, fixture::kTrackPlainPath,
                  fixture::kTrackPlainTitle, album_id, 2,
                  fixture::kTrackPlainEntity);
  for (std::size_t i = 0; i < plain_artist_ids.size(); ++i) {
    linkTrackArtist(session, track_plain, plain_artist_ids[i],
                    static_cast<std::int64_t>(i));
  }

  const std::int64_t track_null =
      insertTrack(session, root_id, fixture::kTrackNullPath,
                  fixture::kTrackNullTitle, album_id, 3,
                  fixture::kTrackNullEntity);

  insertTrack(session, root_id, fixture::kTrackAlbumNullPath,
              fixture::kTrackAlbumNullTitle, album_null_id, 1,
              fixture::kTrackAlbumNullEntity);

  printEnv(fixture::kEnvRootPath, fixture::kRootPath);
  printEnv(fixture::kEnvAlbumId, album_id);
  printEnv(fixture::kEnvAlbumRef,
           "local-main:album:" + std::to_string(album_id));
  printEnv(fixture::kEnvAlbumEntity, fixture::kAlbumEntity);
  printEnv(fixture::kEnvAlbumNullId, album_null_id);
  printEnv(fixture::kEnvAlbumNullRef,
           "local-main:album:" + std::to_string(album_null_id));
  printEnv(fixture::kEnvTrackRoleId, track_role);
  printEnv(fixture::kEnvTrackRoleRef,
           "local-main:track:" + std::to_string(track_role));
  printEnv(fixture::kEnvTrackRoleEntity, fixture::kTrackRoleEntity);
  printEnv(fixture::kEnvTrackPlainId, track_plain);
  printEnv(fixture::kEnvTrackPlainRef,
           "local-main:track:" + std::to_string(track_plain));
  printEnv(fixture::kEnvTrackPlainEntity, fixture::kTrackPlainEntity);
  printEnv(fixture::kEnvTrackNullId, track_null);
  printEnv(fixture::kEnvTrackNullRef,
           "local-main:track:" + std::to_string(track_null));
}

} // namespace

int main(int argc, char **argv) {
  std::string action;
  std::string config_file = "config.json";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config") {
      if (i + 1 >= argc) {
        std::cerr << "--config 缺少参数值\n";
        return 2;
      }
      config_file = argv[++i];
    } else if (action.empty()) {
      action = arg;
    } else {
      std::cerr << "未知参数: " << arg << "\n";
      return 2;
    }
  }
  if (action != "seed" && action != "cleanup") {
    std::cerr << "用法: " << argv[0]
              << " seed|cleanup [--config PATH]\n";
    return 2;
  }

  try {
    const config::Config &cfg = config::load(config_file);
    db::init(cfg.database);
    db::Session &session = db::mainSession();
    session.begin(db::TransactionMode::Immediate);
    if (action == "seed") {
      seed(session);
    } else {
      cleanup(session);
    }
    session.commit();
    db::close();
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "test_fixture 失败: " << ex.what() << "\n";
    return 1;
  }
}