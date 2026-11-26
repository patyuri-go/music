// 假 PostgreSQL 服务端：让集成测试在没有真实数据库的环境里也能跑起来。
//
// 只实现 libpq 实际用到的那一小段 wire protocol：
//   SSLRequest（回 'N'，逼客户端走明文）→ StartupMessage → AuthenticationOk
//   → ParameterStatus / BackendKeyData → ReadyForQuery
//   之后是 Simple Query（'Q'）与扩展查询（Parse/Bind/Describe/Execute/Sync）。
//
// SQL 按规范化文本做白名单匹配：认识的就返回预置结果集，不认识的回
// CommandComplete + 空结果集，并把 SQL 打到 stderr，便于按日志逐步补齐。
//
// 用法：
//   fake_pg --port 0        # 由内核分配端口，启动后打印 "listening <port>"
//   fake_pg --port 15432
//
// 它只依赖 POSIX socket，不链接 Drogon / SOCI。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../fixture_ids.h"

namespace {

constexpr std::int32_t kSslRequest = 80877103;
constexpr std::int32_t kCancelRequest = 80877102;
constexpr std::int32_t kProtocolVersion3 = 196608;

// schema 版本（与 src/schema.h 的 kVersion 对齐时会由写库语句推进，
// 这里固定为当前版本，让启动链路的「版本一致」分支直接通过）。
constexpr const char *kSchemaVersion = "11";

// ---------------------------------------------------------------------------
// 字节序与 IO
// ---------------------------------------------------------------------------

bool readExact(int fd, void *buf, std::size_t len) {
  auto *out = static_cast<char *>(buf);
  std::size_t done = 0;
  while (done < len) {
    const ssize_t n = ::recv(fd, out + done, len - done, 0);
    if (n <= 0) {
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  return true;
}

bool writeAll(int fd, const void *buf, std::size_t len) {
  const auto *in = static_cast<const char *>(buf);
  std::size_t done = 0;
  while (done < len) {
    const ssize_t n = ::send(fd, in + done, len - done, MSG_NOSIGNAL);
    if (n <= 0) {
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  return true;
}

void putInt32(std::string &out, std::int32_t value) {
  const std::uint32_t be = htonl(static_cast<std::uint32_t>(value));
  out.append(reinterpret_cast<const char *>(&be), 4);
}

void putInt16(std::string &out, std::int16_t value) {
  const std::uint16_t be = htons(static_cast<std::uint16_t>(value));
  out.append(reinterpret_cast<const char *>(&be), 2);
}

std::int32_t getInt32(const std::string &data, std::size_t pos) {
  if (pos + 4 > data.size()) {
    return 0;
  }
  std::uint32_t be = 0;
  std::memcpy(&be, data.data() + pos, 4);
  return static_cast<std::int32_t>(ntohl(be));
}

std::int16_t getInt16(const std::string &data, std::size_t pos) {
  if (pos + 2 > data.size()) {
    return 0;
  }
  const auto hi = static_cast<std::uint16_t>(
      static_cast<unsigned char>(data[pos]));
  const auto lo = static_cast<std::uint16_t>(
      static_cast<unsigned char>(data[pos + 1]));
  return static_cast<std::int16_t>((hi << 8) | lo);
}

// 一帧：1 字节类型 + int32 长度（含自身，不含类型）。
bool sendMessage(int fd, char type, const std::string &payload) {
  std::string frame;
  frame.push_back(type);
  putInt32(frame, static_cast<std::int32_t>(payload.size() + 4));
  frame += payload;
  return writeAll(fd, frame.data(), frame.size());
}

// ---------------------------------------------------------------------------
// 结果集
// ---------------------------------------------------------------------------

// 常用类型 OID。
enum : std::int32_t {
  kInt4 = 23,
  kInt8 = 20,
  kText = 25,
  kBool = 16,
};

struct Column {
  std::string name;
  std::int32_t oid;
};

struct Result {
  std::vector<Column> columns;
  std::vector<std::vector<std::string>> rows;
  std::string tag;
};

std::string upper(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return text;
}

// 把连续空白压成一个空格，便于按前缀匹配。
std::string normalize(const std::string &sql) {
  std::string out;
  bool space = false;
  for (const char c : sql) {
    const bool is_space = std::isspace(static_cast<unsigned char>(c)) != 0;
    if (is_space) {
      space = true;
      continue;
    }
    if (space && !out.empty()) {
      out.push_back(' ');
    }
    space = false;
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

bool startsWith(const std::string &text, const std::string &prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}

Result emptyCommand(const std::string &normalized) {
  Result result;
  if (startsWith(normalized, "INSERT")) {
    // 插入 schema_version 后版本就是当前版本，后续查询直接命中。
    result.tag = "INSERT 0 1";
  } else if (startsWith(normalized, "UPDATE")) {
    result.tag = "UPDATE 0";
  } else if (startsWith(normalized, "DELETE")) {
    result.tag = "DELETE 0";
  } else if (startsWith(normalized, "BEGIN")) {
    result.tag = "BEGIN";
  } else if (startsWith(normalized, "COMMIT") ||
             startsWith(normalized, "END")) {
    result.tag = "COMMIT";
  } else if (startsWith(normalized, "ROLLBACK")) {
    result.tag = "ROLLBACK";
  } else if (startsWith(normalized, "CREATE TABLE")) {
    result.tag = "CREATE TABLE";
  } else if (startsWith(normalized, "CREATE INDEX") ||
             startsWith(normalized, "CREATE UNIQUE INDEX")) {
    result.tag = "CREATE INDEX";
  } else if (startsWith(normalized, "ALTER TABLE")) {
    result.tag = "ALTER TABLE";
  } else if (startsWith(normalized, "DROP TABLE")) {
    result.tag = "DROP TABLE";
  } else if (startsWith(normalized, "SET")) {
    result.tag = "SET";
  } else {
    result.tag = "OK";
  }
  return result;
}

// ---------------------------------------------------------------------------
// 夹具数据
// ---------------------------------------------------------------------------

// 固定 id 与 test/fixture_ids.h 对齐。假 PG 不执行 INSERT，所有行都写死
// 在这里；run_integration.sh 用 test_fixture env 打印同样的 id。
namespace fx {

constexpr int kRootId = 1;
constexpr int kAlbumOne = 2;
constexpr int kAlbumNull = 3;
constexpr int kTrackRole = 10;
constexpr int kTrackPlain = 11;
constexpr int kTrackNull = 12;
constexpr int kTrackAlbumNull = 13;
constexpr int kTrackUnlinkedNull = 14;
constexpr int kRoleArtistBase = 4;   // 4..7
constexpr int kPlainArtistBase = 8;  // 8..9

// 夹具的两条跨库链接。
//   1 号：本地 12 号曲目（entity_id 为 NULL）↔ 网易云 186016
//          ——「本地缺实体 → 用链接换源」。
//   2 号：本地 10 号曲目（entity_id 非空）↔ 网易云 186018
//          ——「本地能解析、但存储读不到字节」时是否换源。
// 一条链接每个库最多一个成员（UNIQUE(link_id, library_id)），
// 所以两条链接必须用不同的远端 id。
constexpr int kLinkId = 1;
inline const std::string kLinkLocalRef = "12";
inline const std::string kLinkRemoteRef = fixture::kNeteaseSongId;
constexpr int kLinkIdWithEntity = 2;
inline const std::string kLinkLocalEntityRef = "10";
inline const std::string kLinkRemoteEntityRef = fixture::kNeteaseEntitySongId;
inline const std::string kLinkCreatedAt = "2024-01-01 00:00:00";

struct LinkRow {
  int id;
  const std::string &local_entity;
  const std::string &remote_entity;
};

inline const std::vector<LinkRow> &links() {
  static const std::vector<LinkRow> rows = {
      {kLinkId, kLinkLocalRef, kLinkRemoteRef},
      {kLinkIdWithEntity, kLinkLocalEntityRef, kLinkRemoteEntityRef},
  };
  return rows;
}

inline const LinkRow *link(int id) {
  for (const auto &row : links()) {
    if (row.id == id) {
      return &row;
    }
  }
  return nullptr;
}

// 由任一成员定位链接：locate_link 的查询条件。
inline const LinkRow *link_by_member(const std::string &library,
                                     const std::string &entity_id) {
  for (const auto &row : links()) {
    if (library == "local-main" && entity_id == row.local_entity) {
      return &row;
    }
    if (library == "netease-main" && entity_id == row.remote_entity) {
      return &row;
    }
  }
  return nullptr;
}

inline const std::string kAddedAt = "2024-01-01 00:00:00";

struct ArtistRow {
  int id;
  std::string name;
};

inline const std::vector<ArtistRow> &artists() {
  static const std::vector<ArtistRow> rows = [] {
    std::vector<ArtistRow> out;
    for (std::size_t i = 0; i < fixture::kRoleArtists.size(); ++i) {
      out.push_back({kRoleArtistBase + static_cast<int>(i),
                     fixture::kRoleArtists[i]});
    }
    for (std::size_t i = 0; i < fixture::kPlainArtists.size(); ++i) {
      out.push_back({kPlainArtistBase + static_cast<int>(i),
                     fixture::kPlainArtists[i]});
    }
    return out;
  }();
  return rows;
}

inline std::optional<std::string> artist_name(int id) {
  for (const auto &row : artists()) {
    if (row.id == id) {
      return row.name;
    }
  }
  return std::nullopt;
}

struct AlbumRow {
  int id;
  std::string name;
  int year;
  std::string cover_path;
  std::string artist_key;
  std::optional<std::string> entity_id;
};

inline const std::vector<AlbumRow> &albums() {
  static const std::vector<AlbumRow> rows = {
      {kAlbumOne, fixture::kAlbumName, 2024, fixture::kAlbumCoverPath,
       fixture::kAlbumArtistKey, fixture::kAlbumEntity},
      {kAlbumNull, fixture::kAlbumNullName, 0, "",
       fixture::kAlbumNullArtistKey, std::nullopt},
  };
  return rows;
}

inline const AlbumRow *album(int id) {
  for (const auto &row : albums()) {
    if (row.id == id) {
      return &row;
    }
  }
  return nullptr;
}

struct TrackRow {
  int id;
  int root_id;
  std::string path;
  std::string title;
  int album_id;
  int disc_no;
  int track_no;
  int duration_ms;
  int bitrate;
  int sample_rate;
  int channels;
  std::string format;
  std::int64_t size_bytes;
  std::int64_t mtime;
  std::string added_at;
  int play_count;
  std::optional<std::string> last_played;
  int rating;
  std::optional<std::string> entity_id;
};

inline const std::vector<TrackRow> &tracks() {
  static const std::vector<TrackRow> rows = {
      {kTrackRole, kRootId, fixture::kTrackRolePath, fixture::kTrackRoleTitle,
       kAlbumOne, 1, 1, fixture::kDurationMs, 1411200, 44100, 2,
       fixture::kFormat, fixture::kSizeBytes, 0, kAddedAt, 0, std::nullopt, 0,
       fixture::kTrackRoleEntity},
      {kTrackPlain, kRootId, fixture::kTrackPlainPath,
       fixture::kTrackPlainTitle, kAlbumOne, 1, 2, fixture::kDurationMs,
       1411200, 44100, 2, fixture::kFormat, fixture::kSizeBytes, 0, kAddedAt, 0,
       std::nullopt, 0, fixture::kTrackPlainEntity},
      {kTrackNull, kRootId, fixture::kTrackNullPath, fixture::kTrackNullTitle,
       kAlbumOne, 1, 3, fixture::kDurationMs, 1411200, 44100, 2,
       fixture::kFormat, fixture::kSizeBytes, 0, kAddedAt, 0, std::nullopt, 0,
       std::nullopt},
      {kTrackAlbumNull, kRootId, fixture::kTrackAlbumNullPath,
       fixture::kTrackAlbumNullTitle, kAlbumNull, 1, 1, fixture::kDurationMs,
       1411200, 44100, 2, fixture::kFormat, fixture::kSizeBytes, 0, kAddedAt, 0,
       std::nullopt, 0, fixture::kTrackAlbumNullEntity},
      {kTrackUnlinkedNull, kRootId, fixture::kTrackUnlinkedNullPath,
       fixture::kTrackUnlinkedNullTitle, kAlbumOne, 1, 4, fixture::kDurationMs,
       1411200, 44100, 2, fixture::kFormat, fixture::kSizeBytes, 0, kAddedAt, 0,
       std::nullopt, 0, std::nullopt},
  };
  return rows;
}

inline const TrackRow *track(int id) {
  for (const auto &row : tracks()) {
    if (row.id == id) {
      return &row;
    }
  }
  return nullptr;
}

inline const TrackRow *track_by_path(const std::string &path) {
  for (const auto &row : tracks()) {
    if (row.path == path) {
      return &row;
    }
  }
  return nullptr;
}

inline std::vector<int> album_artist_ids(int album_id) {
  if (album_id != kAlbumOne) {
    return {};
  }
  return {kRoleArtistBase, kRoleArtistBase + 1, kRoleArtistBase + 2,
          kRoleArtistBase + 3};
}

inline std::vector<int> track_artist_ids(int track_id) {
  if (track_id == kTrackRole) {
    return album_artist_ids(kAlbumOne);
  }
  if (track_id == kTrackPlain) {
    return {kPlainArtistBase, kPlainArtistBase + 1};
  }
  return {};
}

inline bool has_lyrics(int track_id) { return track_id == kTrackRole; }

} // namespace fx

using Params = std::vector<std::optional<std::string>>;

std::string param(const Params &params, std::size_t index) {
  if (index >= params.size() || !params[index].has_value()) {
    return {};
  }
  return *params[index];
}

int param_int(const Params &params, std::size_t index) {
  const std::string text = param(params, index);
  return text.empty() ? 0 : std::atoi(text.c_str());
}

std::string to_text(int value) { return std::to_string(value); }

std::string to_text(std::int64_t value) { return std::to_string(value); }

// 按值过滤的单行结果；找不到就返回空结果集。
Result single(const std::vector<Column> &columns,
              const std::vector<std::string> &row, std::string tag) {
  return Result{columns, {row}, std::move(tag)};
}

// 白名单。未命中的 SELECT 返回空结果集（0 行 0 列）并打日志。
Result resolve(const std::string &sql, const Params &params) {
  const std::string norm = normalize(sql);

  if (norm == "SELECT 1") {
    return Result{{{"?column?", kInt4}}, {{"1"}}, "SELECT 1"};
  }
  if (startsWith(norm, "SELECT VERSION FROM SCHEMA_VERSION")) {
    return Result{{{"version", kInt4}}, {{kSchemaVersion}}, "SELECT 1"};
  }

  // ---- playlist::matchSong / playlist::loadAll 的歌名查询 ----
  if (startsWith(norm, "SELECT ID, TITLE, ALBUM_ID, DURATION_MS FROM TRACK")) {
    const std::vector<Column> columns = {{"id", kInt8},
                                         {"title", kText},
                                         {"album_id", kInt8},
                                         {"duration_ms", kInt4}};
    const fx::TrackRow *row = fx::track_by_path(param(params, 0));
    if (row == nullptr) {
      return Result{columns, {}, "SELECT 0"};
    }
    return single(columns,
                  {to_text(static_cast<std::int64_t>(row->id)), row->title,
                   to_text(static_cast<std::int64_t>(row->album_id)),
                   to_text(row->duration_ms)},
                  "SELECT 1");
  }
  if (startsWith(norm, "SELECT NAME FROM ALBUM WHERE ID")) {
    const std::vector<Column> columns = {{"name", kText}};
    const fx::AlbumRow *row = fx::album(param_int(params, 0));
    if (row == nullptr) {
      return Result{columns, {}, "SELECT 0"};
    }
    return single(columns, {row->name}, "SELECT 1");
  }
  if (startsWith(norm,
                 "SELECT A.NAME FROM ARTIST A JOIN TRACK_ARTIST TA")) {
    const std::vector<Column> columns = {{"name", kText}};
    std::vector<std::vector<std::string>> rows;
    for (const int artist_id : fx::track_artist_ids(param_int(params, 0))) {
      const auto name = fx::artist_name(artist_id);
      if (name.has_value()) {
        rows.push_back({*name});
      }
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }
  if (startsWith(norm,
                 "SELECT A.NAME FROM ARTIST A JOIN ALBUM_ARTIST AA")) {
    const std::vector<Column> columns = {{"name", kText}};
    std::vector<std::vector<std::string>> rows;
    for (const int artist_id : fx::album_artist_ids(param_int(params, 0))) {
      const auto name = fx::artist_name(artist_id);
      if (name.has_value()) {
        rows.push_back({*name});
      }
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }

  // ---- 批量补充查询：曲风 / 艺术家 / 歌词 / 专辑艺术家 ----
  // 参数是 IN (...) 里的 id 列表，按夹具数据逐 id 展开；没数据的 id 不出行。
  if (startsWith(norm, "SELECT TG.TRACK_ID, G.NAME FROM TRACK_GENRE")) {
    // 夹具没有曲风数据：列要对，行数为 0 才是正确语义。
    return Result{{{"track_id", kInt8}, {"name", kText}}, {}, "SELECT 0"};
  }
  if (startsWith(norm,
                 "SELECT TA.TRACK_ID, A.ID, A.NAME FROM TRACK_ARTIST")) {
    const std::vector<Column> columns = {
        {"track_id", kInt8}, {"id", kInt8}, {"name", kText}};
    std::vector<std::vector<std::string>> rows;
    for (std::size_t i = 0; i < params.size(); ++i) {
      const int track_id = param_int(params, i);
      for (const int artist_id : fx::track_artist_ids(track_id)) {
        const auto name = fx::artist_name(artist_id);
        if (name.has_value()) {
          rows.push_back({to_text(static_cast<std::int64_t>(track_id)),
                          to_text(static_cast<std::int64_t>(artist_id)),
                          *name});
        }
      }
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }
  if (startsWith(norm, "SELECT TRACK_ID FROM TRACK_LYRICS WHERE TRACK_ID IN")) {
    const std::vector<Column> columns = {{"track_id", kInt8}};
    std::vector<std::vector<std::string>> rows;
    for (std::size_t i = 0; i < params.size(); ++i) {
      const int track_id = param_int(params, i);
      if (fx::has_lyrics(track_id)) {
        rows.push_back({to_text(static_cast<std::int64_t>(track_id))});
      }
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }
  if (startsWith(norm,
                 "SELECT AA.ALBUM_ID, A.ID, A.NAME FROM ALBUM_ARTIST AA")) {
    const std::vector<Column> columns = {
        {"album_id", kInt8}, {"id", kInt8}, {"name", kText}};
    std::vector<std::vector<std::string>> rows;
    for (std::size_t i = 0; i < params.size(); ++i) {
      const int album_id = param_int(params, i);
      for (const int artist_id : fx::album_artist_ids(album_id)) {
        const auto name = fx::artist_name(artist_id);
        if (name.has_value()) {
          rows.push_back({to_text(static_cast<std::int64_t>(album_id)),
                          to_text(static_cast<std::int64_t>(artist_id)),
                          *name});
        }
      }
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }

  // ---- LocalLibrary::snapshot() 的批量查询 ----
  if (startsWith(norm, "SELECT ID, NAME FROM ARTIST") &&
      !startsWith(norm, "SELECT ID, NAME, SORT_NAME")) {
    const std::vector<Column> columns = {{"id", kInt8}, {"name", kText}};
    std::vector<std::vector<std::string>> rows;
    for (const auto &row : fx::artists()) {
      rows.push_back({to_text(static_cast<std::int64_t>(row.id)), row.name});
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }
  if (startsWith(norm,
                 "SELECT ALBUM_ID, ARTIST_ID FROM ALBUM_ARTIST ORDER BY")) {
    const std::vector<Column> columns = {{"album_id", kInt8},
                                         {"artist_id", kInt8}};
    std::vector<std::vector<std::string>> rows;
    for (const int artist_id : fx::album_artist_ids(fx::kAlbumOne)) {
      rows.push_back(
          {to_text(static_cast<std::int64_t>(fx::kAlbumOne)),
           to_text(static_cast<std::int64_t>(artist_id))});
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }
  if (startsWith(norm,
                 "SELECT TRACK_ID, ARTIST_ID FROM TRACK_ARTIST ORDER BY")) {
    const std::vector<Column> columns = {{"track_id", kInt8},
                                         {"artist_id", kInt8}};
    std::vector<std::vector<std::string>> rows;
    for (const int track_id :
         {fx::kTrackRole, fx::kTrackPlain, fx::kTrackNull, fx::kTrackAlbumNull}) {
      for (const int artist_id : fx::track_artist_ids(track_id)) {
        rows.push_back({to_text(static_cast<std::int64_t>(track_id)),
                        to_text(static_cast<std::int64_t>(artist_id))});
      }
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }
  if (startsWith(norm,
                 "SELECT TG.TRACK_ID, G.ID, G.NAME FROM TRACK_GENRE")) {
    return Result{{{"track_id", kInt8}, {"id", kInt8}, {"name", kText}},
                  {},
                  "SELECT 0"};
  }
  if (startsWith(norm, "SELECT ALBUM_ID, COUNT(*), COALESCE(SUM(DURATION_MS)")) {
    const std::vector<Column> columns = {{"album_id", kInt8},
                                         {"count", kInt8},
                                         {"sum", kInt8}};
    std::vector<std::vector<std::string>> rows;
    for (const auto &album : fx::albums()) {
      int count = 0;
      std::int64_t sum = 0;
      for (const auto &track : fx::tracks()) {
        if (track.album_id == album.id) {
          ++count;
          sum += track.duration_ms;
        }
      }
      if (count > 0) {
        rows.push_back({to_text(static_cast<std::int64_t>(album.id)),
                        to_text(static_cast<std::int64_t>(count)),
                        to_text(sum)});
      }
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }
  if (startsWith(norm, "SELECT TRACK_ID FROM TRACK_LYRICS WHERE TEXT")) {
    const std::vector<Column> columns = {{"track_id", kInt8}};
    std::vector<std::vector<std::string>> rows;
    for (const auto &track : fx::tracks()) {
      if (fx::has_lyrics(track.id)) {
        rows.push_back({to_text(static_cast<std::int64_t>(track.id))});
      }
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }
  if (startsWith(norm, "SELECT A.ID, A.NAME, A.YEAR, A.COVER_PATH FROM ALBUM")) {
    const std::vector<Column> columns = {{"id", kInt8},
                                         {"name", kText},
                                         {"year", kInt4},
                                         {"cover_path", kText}};
    std::vector<std::vector<std::string>> rows;
    for (const auto &album : fx::albums()) {
      rows.push_back({to_text(static_cast<std::int64_t>(album.id)), album.name,
                      to_text(album.year), album.cover_path});
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }
  if (startsWith(norm, "SELECT ID, NAME, SORT_NAME FROM ARTIST ORDER BY")) {
    const std::vector<Column> columns = {
        {"id", kInt8}, {"name", kText}, {"sort_name", kText}};
    std::vector<fx::ArtistRow> sorted = fx::artists();
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const fx::ArtistRow &a, const fx::ArtistRow &b) {
                       return a.name < b.name;
                     });
    std::vector<std::vector<std::string>> rows;
    for (const auto &row : sorted) {
      rows.push_back({to_text(static_cast<std::int64_t>(row.id)), row.name, ""});
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }
  if (startsWith(norm,
                 "SELECT ID, ROOT_ID, PATH, TITLE, ALBUM_ID, DISC_NO, TRACK_NO")) {
    const std::vector<Column> columns = {
        {"id", kInt8},          {"root_id", kInt8},     {"path", kText},
        {"title", kText},       {"album_id", kInt8},    {"disc_no", kInt4},
        {"track_no", kInt4},    {"duration_ms", kInt4}, {"bitrate", kInt4},
        {"sample_rate", kInt4}, {"channels", kInt4},    {"format", kText},
        {"size_bytes", kInt8},  {"mtime", kInt8},       {"added_at", kText},
        {"play_count", kInt4},  {"last_played", kText}, {"rating", kInt4}};
    std::vector<fx::TrackRow> sorted = fx::tracks();
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const fx::TrackRow &a, const fx::TrackRow &b) {
                       if (a.album_id != b.album_id) return a.album_id < b.album_id;
                       if (a.disc_no != b.disc_no) return a.disc_no < b.disc_no;
                       if (a.track_no != b.track_no) return a.track_no < b.track_no;
                       return a.id < b.id;
                     });
    std::vector<std::vector<std::string>> rows;
    for (const auto &track : sorted) {
      rows.push_back({
          to_text(static_cast<std::int64_t>(track.id)),
          to_text(static_cast<std::int64_t>(track.root_id)), track.path,
          track.title,
          to_text(static_cast<std::int64_t>(track.album_id)),
          to_text(track.disc_no), to_text(track.track_no),
          to_text(track.duration_ms), to_text(track.bitrate),
          to_text(track.sample_rate), to_text(track.channels), track.format,
          to_text(track.size_bytes), to_text(track.mtime), track.added_at,
          to_text(track.play_count), track.last_played.value_or(""),
          to_text(track.rating)});
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }
  if (startsWith(norm,
                 "SELECT T.ID FROM TRACK T JOIN PLAY_EVENT PE")) {
    return Result{{{"id", kInt8}}, {}, "SELECT 0"};
  }

  // ---- 封面 / 歌词 / 实体链接 ----
  if (startsWith(norm, "SELECT COVER_PATH, ENTITY_ID FROM ALBUM WHERE ID")) {
    const std::vector<Column> columns = {{"cover_path", kText},
                                         {"entity_id", kText}};
    const fx::AlbumRow *row = fx::album(param_int(params, 0));
    if (row == nullptr) {
      return Result{columns, {}, "SELECT 0"};
    }
    return single(columns, {row->cover_path, row->entity_id.value_or("")},
                  "SELECT 1");
  }
  if (startsWith(norm, "SELECT AL.COVER_PATH, AL.ENTITY_ID FROM ALBUM AL JOIN "
                       "ALBUM_ARTIST AA")) {
    const std::vector<Column> columns = {{"cover_path", kText},
                                         {"entity_id", kText}};
    // 只有专辑一的 4 位艺术家挂了专辑；其余艺术家没有封面。
    const int artist_id = param_int(params, 0);
    const fx::AlbumRow *row = nullptr;
    if (artist_id >= fx::kRoleArtistBase &&
        artist_id < fx::kRoleArtistBase + 4) {
      row = fx::album(fx::kAlbumOne);
    }
    if (row == nullptr || row->cover_path.empty()) {
      return Result{columns, {}, "SELECT 0"};
    }
    return single(columns, {row->cover_path, row->entity_id.value_or("")},
                  "SELECT 1");
  }
  if (startsWith(norm, "SELECT TEXT, SYNCED FROM TRACK_LYRICS WHERE TRACK_ID")) {
    const std::vector<Column> columns = {{"text", kText}, {"synced", kInt4}};
    const int track_id = param_int(params, 0);
    if (!fx::has_lyrics(track_id)) {
      return Result{columns, {}, "SELECT 0"};
    }
    return single(columns, {std::string(fixture::kSyncedLyrics), "1"},
                  "SELECT 1");
  }
  // ---- 跨库链接：两条（本地 12→186016、本地 10→186018）----
  if (startsWith(norm,
                 "SELECT L.LINK_ID, L.ENTITY_TYPE FROM ENTITY_LINK_MEMBER")) {
    const std::vector<Column> columns = {{"link_id", kInt8},
                                         {"entity_type", kText}};
    // 参数顺序：library_id, entity_type, entity_id。
    const std::string library = param(params, 0);
    const std::string entity_type = param(params, 1);
    const std::string entity_id = param(params, 2);
    const fx::LinkRow *row = entity_type == "track"
                                 ? fx::link_by_member(library, entity_id)
                                 : nullptr;
    if (row == nullptr) {
      return Result{columns, {}, "SELECT 0"};
    }
    return single(columns,
                  {to_text(static_cast<std::int64_t>(row->id)), "track"},
                  "SELECT 1");
  }
  if (startsWith(
          norm,
          "SELECT CREATED_AT, UPDATED_AT FROM ENTITY_LINK WHERE LINK_ID")) {
    const std::vector<Column> columns = {{"created_at", kText},
                                         {"updated_at", kText}};
    if (fx::link(param_int(params, 0)) == nullptr) {
      return Result{columns, {}, "SELECT 0"};
    }
    return single(columns, {fx::kLinkCreatedAt, fx::kLinkCreatedAt}, "SELECT 1");
  }
  if (startsWith(norm, "SELECT LIBRARY_ID, ENTITY_TYPE, ENTITY_ID, CONFIDENCE, "
                       "VERIFIED FROM ENTITY_LINK_MEMBER WHERE LINK_ID")) {
    const std::vector<Column> columns = {{"library_id", kText},
                                         {"entity_type", kText},
                                         {"entity_id", kText},
                                         {"confidence", kText},
                                         {"verified", kBool}};
    const fx::LinkRow *row = fx::link(param_int(params, 0));
    if (row == nullptr) {
      return Result{columns, {}, "SELECT 0"};
    }
    // 一条链接每个库最多一个成员（UNIQUE(link_id, library_id)）。
    return Result{columns,
                  {{"local-main", "track", row->local_entity, "manual", "t"},
                   {"netease-main", "track", row->remote_entity, "manual", "t"}},
                  "SELECT 2"};
  }
  if (startsWith(norm,
                 "SELECT LIBRARY_ID FROM ENTITY_LINK_MEMBER WHERE LINK_ID")) {
    const std::vector<Column> columns = {{"library_id", kText}};
    if (fx::link(param_int(params, 0)) == nullptr) {
      return Result{columns, {}, "SELECT 0"};
    }
    return Result{columns, {{"local-main"}, {"netease-main"}}, "SELECT 2"};
  }
  if (startsWith(norm, "SELECT ASPECT, LIBRARY_ID FROM ENTITY_LINK_OVERRIDE "
                       "WHERE LINK_ID")) {
    const std::vector<Column> columns = {{"aspect", kText},
                                         {"library_id", kText}};
    if (fx::link(param_int(params, 0)) == nullptr) {
      return Result{columns, {}, "SELECT 0"};
    }
    // 只有 stream 有 override，并且故意排成「本地优先」：临时配置里
    // library_priority 把 netease 排前面，所以这条断言能证明 override 生效。
    // metadata 没有 override，落到内建顺序，仍走本地。
    return Result{columns,
                  {{"stream", "local-main"}, {"stream", "netease-main"}},
                  "SELECT 2"};
  }
  if (startsWith(norm, "SELECT LIBRARY_ID FROM ENTITY_LINK_OVERRIDE WHERE "
                       "LINK_ID")) {
    const std::vector<Column> columns = {{"library_id", kText}};
    if (fx::link(param_int(params, 0)) == nullptr) {
      return Result{columns, {}, "SELECT 0"};
    }
    const std::string aspect = param(params, 1);
    if (aspect != "stream") {
      return Result{columns, {}, "SELECT 0"};
    }
    return Result{columns, {{"local-main"}, {"netease-main"}}, "SELECT 2"};
  }

  // ---- 单条曲目路径：get_track / build_track / track_transport ----
  if (startsWith(norm, "SELECT T.*, (SELECT STRING_AGG")) {
    const std::vector<Column> columns = {
        {"id", kInt8},          {"root_id", kInt8},     {"path", kText},
        {"title", kText},       {"album_id", kInt8},    {"disc_no", kInt4},
        {"track_no", kInt4},    {"duration_ms", kInt4}, {"bitrate", kInt4},
        {"sample_rate", kInt4}, {"channels", kInt4},    {"format", kText},
        {"size_bytes", kInt8},  {"mtime", kInt8},       {"added_at", kText},
        {"play_count", kInt4},  {"last_played", kText}, {"rating", kInt4},
        {"entity_id", kText},   {"artist_ids", kText}};
    // 同一个 SELECT 前缀被两处复用：
    //   trackById   ... FROM track t WHERE t.id = ?
    //   tracksByAlbum ... FROM track t WHERE t.album_id = ?
    // 按 WHERE 子句区分，否则按 album id 查询会拿不到任何行。
    const bool by_album = norm.find("WHERE T.ALBUM_ID") != std::string::npos;
    const auto all = fx::tracks();
    std::vector<const fx::TrackRow *> rows;
    if (by_album) {
      for (const auto &track : all) {
        if (track.album_id == param_int(params, 0)) {
          rows.push_back(&track);
        }
      }
    } else if (const fx::TrackRow *row = fx::track(param_int(params, 0))) {
      rows.push_back(row);
    }
    if (rows.empty()) {
      return Result{columns, {}, "SELECT 0"};
    }
    std::vector<std::vector<std::string>> out;
    for (const fx::TrackRow *row : rows) {
      std::string artist_ids;
      for (const int artist_id : fx::track_artist_ids(row->id)) {
        if (!artist_ids.empty()) {
          artist_ids.push_back(',');
        }
        artist_ids += to_text(static_cast<std::int64_t>(artist_id));
      }
      out.push_back({to_text(static_cast<std::int64_t>(row->id)),
                     to_text(static_cast<std::int64_t>(row->root_id)),
                     row->path, row->title,
                     to_text(static_cast<std::int64_t>(row->album_id)),
                     to_text(row->disc_no), to_text(row->track_no),
                     to_text(row->duration_ms), to_text(row->bitrate),
                     to_text(row->sample_rate), to_text(row->channels),
                     row->format, to_text(row->size_bytes),
                     to_text(row->mtime), row->added_at,
                     to_text(row->play_count), row->last_played.value_or(""),
                     to_text(row->rating), row->entity_id.value_or(""),
                     artist_ids});
    }
    return Result{columns, std::move(out),
                  "SELECT " + std::to_string(out.size())};
  }
  if (startsWith(norm, "SELECT PATH, MTIME, ALBUM_ID FROM TRACK WHERE ID")) {
    const std::vector<Column> columns = {
        {"path", kText}, {"mtime", kInt8}, {"album_id", kInt8}};
    const fx::TrackRow *row = fx::track(param_int(params, 0));
    if (row == nullptr) {
      return Result{columns, {}, "SELECT 0"};
    }
    return single(columns,
                  {row->path, to_text(row->mtime),
                   to_text(static_cast<std::int64_t>(row->album_id))},
                  "SELECT 1");
  }
  if (startsWith(norm, "SELECT ARTIST_ID FROM TRACK_ARTIST WHERE TRACK_ID")) {
    const std::vector<Column> columns = {{"artist_id", kInt8}};
    std::vector<std::vector<std::string>> rows;
    for (const int artist_id : fx::track_artist_ids(param_int(params, 0))) {
      rows.push_back({to_text(static_cast<std::int64_t>(artist_id))});
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }
  if (startsWith(norm, "SELECT 1 FROM TRACK_LYRICS WHERE TRACK_ID")) {
    const std::vector<Column> columns = {{"?column?", kInt4}};
    if (!fx::has_lyrics(param_int(params, 0))) {
      return Result{columns, {}, "SELECT 0"};
    }
    return single(columns, {"1"}, "SELECT 1");
  }
  if (startsWith(norm,
                 "SELECT G.NAME FROM TRACK_GENRE TG JOIN GENRE G ON G.ID")) {
    return Result{{{"name", kText}}, {}, "SELECT 0"};
  }
  if (startsWith(norm, "SELECT * FROM ARTIST WHERE ID")) {
    const std::vector<Column> columns = {
        {"id", kInt8}, {"name", kText}, {"sort_name", kText}};
    for (const auto &row : fx::artists()) {
      if (row.id == param_int(params, 0)) {
        return single(columns,
                      {to_text(static_cast<std::int64_t>(row.id)), row.name, ""},
                      "SELECT 1");
      }
    }
    return Result{columns, {}, "SELECT 0"};
  }
  if (startsWith(norm, "SELECT A.*, (SELECT STRING_AGG")) {
    const std::vector<Column> columns = {
        {"id", kInt8},        {"name", kText},      {"year", kInt4},
        {"cover_path", kText}, {"artist_key", kText}, {"entity_id", kText},
        {"artist_ids", kText}};
    const fx::AlbumRow *row = fx::album(param_int(params, 0));
    if (row == nullptr) {
      return Result{columns, {}, "SELECT 0"};
    }
    std::string artist_ids;
    for (const int artist_id : fx::album_artist_ids(row->id)) {
      if (!artist_ids.empty()) {
        artist_ids.push_back(',');
      }
      artist_ids += to_text(static_cast<std::int64_t>(artist_id));
    }
    return single(columns,
                  {to_text(static_cast<std::int64_t>(row->id)), row->name,
                   to_text(row->year), row->cover_path, row->artist_key,
                   row->entity_id.value_or(""), artist_ids},
                  "SELECT 1");
  }
  if (startsWith(norm, "SELECT ARTIST_ID FROM ALBUM_ARTIST WHERE ALBUM_ID")) {
    const std::vector<Column> columns = {{"artist_id", kInt8}};
    std::vector<std::vector<std::string>> rows;
    for (const int artist_id : fx::album_artist_ids(param_int(params, 0))) {
      rows.push_back({to_text(static_cast<std::int64_t>(artist_id))});
    }
    return Result{columns, rows, "SELECT " + std::to_string(rows.size())};
  }

  if (startsWith(norm, "SELECT") || startsWith(norm, "SHOW") ||
      startsWith(norm, "WITH")) {
    std::fprintf(stderr, "[fake_pg] unhandled query: %s\n", norm.c_str());
    return Result{{}, {}, "SELECT 0"};
  }
  return emptyCommand(norm);
}

// ---------------------------------------------------------------------------
// 协议编码
// ---------------------------------------------------------------------------

std::string rowDescription(const Result &result) {
  std::string out;
  putInt16(out, static_cast<std::int16_t>(result.columns.size()));
  for (const auto &column : result.columns) {
    out += column.name;
    out.push_back('\0');
    putInt32(out, 0);                                    // table oid
    putInt16(out, 0);                                    // column attr
    putInt32(out, column.oid);                           // type oid
    putInt16(out, column.oid == kText ? -1 : 4);         // typlen
    putInt32(out, -1);                                   // typmod
    putInt16(out, 0);                                    // format = text
  }
  return out;
}

std::string dataRow(const std::vector<std::string> &row) {
  std::string out;
  putInt16(out, static_cast<std::int16_t>(row.size()));
  for (const auto &value : row) {
    putInt32(out, static_cast<std::int32_t>(value.size()));
    out += value;
  }
  return out;
}

std::string commandComplete(const std::string &tag) {
  std::string out = tag;
  out.push_back('\0');
  return out;
}

void sendReady(int fd) { sendMessage(fd, 'Z', std::string(1, 'I')); }

void sendRowDescription(int fd, const Result &result) {
  if (result.columns.empty()) {
    sendMessage(fd, 'n', "");  // NoData
    return;
  }
  sendMessage(fd, 'T', rowDescription(result));
}

void sendRows(int fd, const Result &result) {
  for (const auto &row : result.rows) {
    sendMessage(fd, 'D', dataRow(row));
  }
  sendMessage(fd, 'C', commandComplete(result.tag));
}

// ---------------------------------------------------------------------------
// 会话
// ---------------------------------------------------------------------------

std::string readCString(const std::string &data, std::size_t &pos) {
  const std::size_t end = data.find('\0', pos);
  if (end == std::string::npos) {
    pos = data.size();
    return {};
  }
  std::string out = data.substr(pos, end - pos);
  pos = end + 1;
  return out;
}

struct Session {
  int fd = -1;
  std::map<std::string, std::string> statements;  // name -> sql
  std::map<std::string, std::string> portals;     // portal -> statement
  std::map<std::string, std::vector<std::optional<std::string>>>
      portal_params;  // portal -> 绑定参数（文本格式）
};

enum class Startup { kAgain, kReady, kFatal };

Startup handleStartup(Session &session) {
  std::uint32_t raw_len = 0;
  if (!readExact(session.fd, &raw_len, 4)) {
    return Startup::kFatal;
  }
  const std::int32_t length = static_cast<std::int32_t>(ntohl(raw_len));
  if (length < 8) {
    return Startup::kFatal;
  }
  std::string payload(static_cast<std::size_t>(length) - 4, '\0');
  if (!readExact(session.fd, payload.data(), payload.size())) {
    return Startup::kFatal;
  }
  const std::int32_t code = getInt32(payload, 0);
  if (code == kSslRequest) {
    // libpq 默认 sslmode=prefer：拒绝 SSL 后它会改用明文重发 StartupMessage。
    const char refuse = 'N';
    return writeAll(session.fd, &refuse, 1) ? Startup::kAgain : Startup::kFatal;
  }
  if (code == kCancelRequest) {
    return Startup::kFatal;
  }
  if (code != kProtocolVersion3) {
    std::fprintf(stderr, "[fake_pg] unknown startup code %d\n", code);
    return Startup::kFatal;
  }

  std::string auth;
  putInt32(auth, 0);  // AuthenticationOk
  if (!sendMessage(session.fd, 'R', auth)) {
    return Startup::kFatal;
  }
  const std::pair<const char *, const char *> params[] = {
      {"server_version", "15.4"},
      {"server_encoding", "UTF8"},
      {"client_encoding", "UTF8"},
      {"DateStyle", "ISO, MDY"},
      {"integer_datetimes", "on"},
      {"standard_conforming_strings", "on"},
      {"TimeZone", "UTC"},
      {"application_name", ""},
  };
  for (const auto &[key, value] : params) {
    std::string status = std::string(key) + '\0' + value + '\0';
    if (!sendMessage(session.fd, 'S', status)) {
      return Startup::kFatal;
    }
  }
  std::string backend;
  putInt32(backend, static_cast<std::int32_t>(::getpid()));
  putInt32(backend, 0x1234);
  if (!sendMessage(session.fd, 'K', backend)) {
    return Startup::kFatal;
  }
  sendReady(session.fd);
  return Startup::kReady;
}

// 返回 false 表示关闭连接。
bool handleMessage(Session &session) {
  char type = 0;
  if (!readExact(session.fd, &type, 1)) {
    return false;
  }
  std::uint32_t raw_len = 0;
  if (!readExact(session.fd, &raw_len, 4)) {
    return false;
  }
  const std::int32_t length = static_cast<std::int32_t>(ntohl(raw_len));
  if (length < 4) {
    return false;
  }
  std::string payload(static_cast<std::size_t>(length) - 4, '\0');
  if (!payload.empty() && !readExact(session.fd, payload.data(), payload.size())) {
    return false;
  }

  switch (type) {
  case 'Q': {  // Simple Query
    const std::string sql = payload.c_str();
    if (std::getenv("FAKE_PG_DEBUG") != nullptr) {
      std::fprintf(stderr, "[fake_pg] Q: %s\n", sql.c_str());
    }
    const Result result = resolve(sql, {});
    if (!result.columns.empty()) {
      sendMessage(session.fd, 'T', rowDescription(result));
      sendRows(session.fd, result);
    } else {
      sendMessage(session.fd, 'C', commandComplete(result.tag));
    }
    sendReady(session.fd);
    return true;
  }
  case 'P': {  // Parse
    std::size_t pos = 0;
    const std::string name = readCString(payload, pos);
    const std::string sql = readCString(payload, pos);
    session.statements[name] = sql;
    if (std::getenv("FAKE_PG_DEBUG") != nullptr) {
      std::fprintf(stderr, "[fake_pg] P: %s\n", sql.c_str());
    }
    sendMessage(session.fd, '1', "");
    return true;
  }
  case 'B': {  // Bind
    std::size_t pos = 0;
    const std::string portal = readCString(payload, pos);
    const std::string statement = readCString(payload, pos);
    session.portals[portal] = statement;

    // 参数格式码：int16 个数 + 每个 int16；当前只接受文本格式(0)，但必须
    // 跳过这些字节才能读到参数值。
    const int format_count = getInt16(payload, pos);
    pos += 2;
    pos += static_cast<std::size_t>(std::max(0, format_count)) * 2;

    const int param_count = getInt16(payload, pos);
    pos += 2;
    std::vector<std::optional<std::string>> params;
    params.reserve(static_cast<std::size_t>(std::max(0, param_count)));
    for (int i = 0; i < param_count; ++i) {
      const std::int32_t length = getInt32(payload, pos);
      pos += 4;
      if (length < 0) {
        params.push_back(std::nullopt);
        continue;
      }
      const auto size = static_cast<std::size_t>(length);
      if (pos + size > payload.size()) {
        params.push_back(std::nullopt);
        pos = payload.size();
        continue;
      }
      params.push_back(payload.substr(pos, size));
      pos += size;
    }
    session.portal_params[portal] = std::move(params);

    sendMessage(session.fd, '2', "");
    return true;
  }
  case 'D': {  // Describe
    if (payload.empty()) {
      return false;
    }
    const char kind = payload[0];
    std::size_t pos = 1;
    const std::string name = readCString(payload, pos);
    const std::string sql =
        kind == 'S' ? session.statements[name]
                    : session.statements[session.portals[name]];
    const Result result =
        kind == 'S' ? resolve(sql, {})
                    : resolve(sql, session.portal_params[name]);
    sendRowDescription(session.fd, result);
    return true;
  }
  case 'E': {  // Execute
    std::size_t pos = 0;
    const std::string portal = readCString(payload, pos);
    const std::string sql = session.statements[session.portals[portal]];
    const Result result = resolve(sql, session.portal_params[portal]);
    sendRows(session.fd, result);
    return true;
  }
  case 'S':  // Sync
    sendReady(session.fd);
    return true;
  case 'H':  // Flush
  case 'C':  // Close：客户端不关心关闭的应答
    if (type == 'C') {
      sendMessage(session.fd, '3', "");
    }
    return true;
  case 'X':  // Terminate
    return false;
  default:
    std::fprintf(stderr, "[fake_pg] unhandled message type %c\n", type);
    return false;
  }
}

void serve(int client) {
  Session session;
  session.fd = client;
  // 关掉 Nagle：扩展查询每条都是几十字节的小包，等 ACK 会把每次往返拖到 40ms。
  int one = 1;
  ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  // 一个连接可能先来 SSLRequest 再来 StartupMessage，所以在握手阶段循环。
  while (true) {
    const Startup step = handleStartup(session);
    if (step == Startup::kFatal) {
      ::close(client);
      return;
    }
    if (step == Startup::kReady) {
      break;
    }
  }
  while (handleMessage(session)) {
  }
  ::close(client);
}

int listenOn(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::perror("[fake_pg] socket");
    return -1;
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    std::perror("[fake_pg] bind");
    ::close(fd);
    return -1;
  }
  if (::listen(fd, 16) < 0) {
    std::perror("[fake_pg] listen");
    ::close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  ::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
  std::printf("listening %d\n", ntohs(addr.sin_port));
  std::fflush(stdout);
  return fd;
}

} // namespace

int main(int argc, char **argv) {
  int port = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = std::atoi(argv[++i]);
    }
  }
  const int server = listenOn(port);
  if (server < 0) {
    return 1;
  }
  ::signal(SIGCHLD, SIG_IGN);
  while (true) {
    const int client = ::accept(server, nullptr, nullptr);
    if (client < 0) {
      continue;
    }
    // 每个连接一个子进程：musicd 会同时持有多个 Session，
    // 单线程串行处理会让后开的连接永远等不到 accept。
    const pid_t pid = ::fork();
    if (pid == 0) {
      ::close(server);
      serve(client);
      ::_exit(0);
    }
    ::close(client);
  }
}