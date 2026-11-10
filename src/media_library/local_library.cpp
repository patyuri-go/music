#include "media_library/local_library.h"

#include "config.h"
#include "db.h"
#include "http_util.h"
#include "schema.h"
#include "storage.h"

#include "api/playlist/playlist.h"
#include "api/webdav/webdav.h"
#include "media_library/lyric_parser.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace media_library {
namespace {

// 搜索游标就是聚合流里的十进制偏移，与网易云库同一约定；空/非法一律当 0。
int search_offset(const std::optional<std::string> &cursor) {
  if (!cursor.has_value() || cursor->empty()) {
    return 0;
  }
  int parsed = 0;
  const char *begin = cursor->data();
  const char *end = begin + cursor->size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end || parsed < 0) {
    return 0;
  }
  return parsed;
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

std::string mime_for(const std::string &format) {
  const std::string lower_format = lower(format);
  if (lower_format == "flac") {
    return "audio/flac";
  }
  if (lower_format == "mp3") {
    return "audio/mpeg";
  }
  if (lower_format == "m4a" || lower_format == "aac") {
    return "audio/mp4";
  }
  if (lower_format == "ogg" || lower_format == "opus") {
    return "audio/ogg";
  }
  if (lower_format == "wav") {
    return "audio/wav";
  }
  return "application/octet-stream";
}

bool contains_name(const std::vector<ArtistLink> &links,
                   const std::string &name) {
  const std::string needle = lower(name);
  return std::any_of(links.begin(), links.end(), [&](const ArtistLink &link) {
    return lower(link.name) == needle;
  });
}

bool contains_name(const std::vector<AlbumArtistLink> &links,
                   const std::string &name) {
  const std::string needle = lower(name);
  return std::any_of(
      links.begin(), links.end(),
      [&](const AlbumArtistLink &link) { return lower(link.name) == needle; });
}

// 结果集按列取值。列不存在时当作 NULL，避免调用方到处做越界判断。
const db::Cell &cell_at(const db::Row &row, std::size_t index) {
  static const db::Cell kNull{db::Null{}};
  return index < row.size() ? row[index] : kNull;
}

bool is_null_at(const db::Row &row, std::size_t index) {
  return cell_at(row, index).is_null();
}

std::int64_t int_at(const db::Row &row, std::size_t index) {
  const db::Cell &cell = cell_at(row, index);
  if (const auto *value = std::get_if<std::int64_t>(&cell.value)) {
    return *value;
  }
  if (const auto *value = std::get_if<double>(&cell.value)) {
    return static_cast<std::int64_t>(*value);
  }
  if (const auto *value = std::get_if<std::string>(&cell.value)) {
    try {
      return std::stoll(*value);
    } catch (const std::exception &) {
      return 0;
    }
  }
  return 0;
}

std::string text_at(const db::Row &row, std::size_t index) {
  const db::Cell &cell = cell_at(row, index);
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

} // namespace

LocalLibrary::LocalLibrary(std::string library_id, std::string display_name)
    : library_id_(std::move(library_id)),
      display_name_(std::move(display_name)) {}

void LocalLibrary::initialize() {
  auto self = shared_from_this();
  register_capability<CatalogReader>(self);
  register_capability<CatalogLister>(self);
  register_capability<PlaylistReader>(self);
  register_capability<Searcher>(self);
  register_capability<StreamResolver>(self);
  register_capability<LyricsProvider>(self);
  register_capability<ArtworkProvider>(self);
  register_capability<ArtworkBytes>(self);
}

LibraryIdentity LocalLibrary::identity() const {
  return LibraryIdentity{library_id_, display_name_, "local"};
}

CapabilitySet LocalLibrary::capabilities() const {
  CapabilitySet set;
  const auto add = [&set](CapabilityId id, bool supported) {
    CapabilityState state;
    state.id = id;
    state.supported = supported;
    state.available = supported;
    set.items.push_back(std::move(state));
  };
  add(CapabilityId::CatalogRead, true);
  add(CapabilityId::CatalogList, true);
  add(CapabilityId::Search, true);
  add(CapabilityId::StreamResolve, true);
  add(CapabilityId::LyricsRead, true);
  add(CapabilityId::ArtworkRead, true);
  add(CapabilityId::PlaylistRead, true);
  add(CapabilityId::PlaylistWrite, false);
  add(CapabilityId::FavoriteRead, false);
  add(CapabilityId::FavoriteWrite, false);
  add(CapabilityId::LibraryWrite, false);
  add(CapabilityId::Auth, false);
  return set;
}

void LocalLibrary::close() noexcept {}

SourceRef LocalLibrary::ref_for(EntityType type, int internal_id) {
  return SourceRef{library_id_, type, std::to_string(internal_id)};
}

std::optional<SourceRef> LocalLibrary::ref_for_compat(EntityType type,
                                                      int internal_id) {
  if (internal_id <= 0) {
    return std::nullopt;
  }
  return ref_for(type, internal_id);
}

std::optional<int> LocalLibrary::internal_id_of(const SourceRef &ref) const {
  if (ref.library_id != library_id_ || ref.entity_id.empty()) {
    return std::nullopt;
  }
  // 对外 id 就是本地库主键的十进制文本：只接受规范形式，避免同一实体
  // 出现 "1" / "01" 两种引用。
  if (ref.entity_id.size() > 1 && ref.entity_id.front() == '0') {
    return std::nullopt;
  }
  for (const char c : ref.entity_id) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return std::nullopt;
    }
  }
  int value = 0;
  const char *begin = ref.entity_id.data();
  const char *end = begin + ref.entity_id.size();
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end || value <= 0) {
    return std::nullopt;
  }
  return value;
}

std::vector<std::string> LocalLibrary::genres_of(int track_id) const {
  const auto result = db().query(
      "SELECT g.name FROM track_genre tg JOIN genre g ON g.id = tg.genre_id "
      "WHERE tg.track_id = ? ORDER BY g.name",
      {static_cast<std::int64_t>(track_id)});
  std::vector<std::string> out;
  out.reserve(result.rows.size());
  for (const auto &row : result.rows) {
    out.push_back(text_at(row, 0));
  }
  return out;
}

std::optional<repo::Album>
LocalLibrary::album_of(const repo::Track &row) const {
  if (row.album_id <= 0) {
    return std::nullopt;
  }
  repo::AlbumRepo albums(db());
  return albums.byId(row.album_id);
}

Track LocalLibrary::build_track(const repo::Track &row) {
  Track track;
  track.ref = ref_for(EntityType::Track, row.id);
  track.title = row.title;
  track.duration_ms = row.duration_ms;
  track.track_no = row.track_no;
  track.disc_no = row.disc_no;
  track.genres = genres_of(row.id);
  track.user_data.play_count = row.play_count;
  track.user_data.last_played = row.last_played;
  track.user_data.rating = row.rating;
  track.user_data.added_at = row.added_at;
  track.audio.bitrate_kbps = row.bitrate;
  track.audio.sample_rate = row.sample_rate;
  track.audio.channels = row.channels;
  track.audio.format = row.format;
  track.audio.size_bytes = row.size_bytes;

  repo::ArtistRepo artists(db());
  for (const int artist_id : row.artist_ids) {
    if (const auto artist = artists.byId(artist_id)) {
      track.artists.push_back(
          ArtistLink{ref_for(EntityType::Artist, artist_id), artist->name});
    }
  }

  if (const auto album = album_of(row)) {
    track.album = AlbumLink{ref_for(EntityType::Album, album->id), album->name};
    track.year = album->year;
    repo::AlbumRepo albums(db());
    for (const int artist_id : albums.artistIdsByAlbumId(album->id)) {
      if (const auto artist = artists.byId(artist_id)) {
        track.album_artists.push_back(AlbumArtistLink{
            ref_for(EntityType::AlbumArtist, artist_id), artist->name});
      }
    }
    const std::string cover = cover_path_of(track.ref);
    if (!cover.empty()) {
      track.artwork = ArtworkRef{
          ref_for(EntityType::Album, album->id), "primary", std::nullopt,
          std::to_string(std::hash<std::string>{}(cover))};
    }
  }

  track.lyrics_available =
      !db().query("SELECT 1 FROM track_lyrics WHERE track_id = ? AND "
                  "text <> ''",
                  {static_cast<std::int64_t>(row.id)})
           .empty();

  return track;
}

Album LocalLibrary::build_album(const repo::Album &row) {
  Album album;
  album.ref = ref_for(EntityType::Album, row.id);
  album.title = row.name;
  album.year = row.year;

  repo::ArtistRepo artists(db());
  for (const int artist_id : row.artist_ids) {
    if (const auto artist = artists.byId(artist_id)) {
      album.artists.push_back(AlbumArtistLink{
          ref_for(EntityType::AlbumArtist, artist_id), artist->name});
    }
  }

  if (!row.cover_path.empty()) {
    album.artwork =
        ArtworkRef{album.ref, "primary", std::nullopt,
                   std::to_string(std::hash<std::string>{}(row.cover_path))};
  }
  return album;
}

Artist LocalLibrary::build_artist(const repo::Artist &row) {
  Artist artist;
  artist.ref = ref_for(EntityType::Artist, row.id);
  artist.name = row.name;
  artist.sort_name = row.sort_name;
  const std::string cover = cover_path_of(artist.ref);
  if (!cover.empty()) {
    artist.artwork =
        ArtworkRef{artist.ref, "primary", std::nullopt,
                   std::to_string(std::hash<std::string>{}(cover))};
  }
  return artist;
}

std::vector<Track>
LocalLibrary::build_tracks(const std::vector<repo::Track> &rows) {
  std::vector<Track> out;
  if (rows.empty()) {
    return out;
  }
  out.reserve(rows.size());

  db::ParamList track_ids;
  track_ids.reserve(rows.size());
  std::vector<int> album_ids;
  std::unordered_set<int> seen_albums;
  for (const repo::Track &row : rows) {
    track_ids.push_back(static_cast<std::int64_t>(row.id));
    if (row.album_id > 0 && seen_albums.insert(row.album_id).second) {
      album_ids.push_back(row.album_id);
    }
  }
  const std::string track_in = schema::sql::placeholders(rows.size());

  std::unordered_map<int, std::vector<std::string>> genres;
  for (const db::Row &row :
       db()
           .query("SELECT tg.track_id, g.name FROM track_genre tg "
                  "JOIN genre g ON g.id = tg.genre_id WHERE tg.track_id "
                  "IN (" +
                      track_in + ") ORDER BY tg.track_id, g.name",
                  track_ids)
           .rows) {
    genres[static_cast<int>(int_at(row, 0))].push_back(text_at(row, 1));
  }

  std::unordered_map<int, std::vector<ArtistLink>> track_artists;
  for (const db::Row &row :
       db()
           .query("SELECT ta.track_id, a.id, a.name FROM track_artist ta "
                  "JOIN artist a ON a.id = ta.artist_id WHERE ta.track_id "
                  "IN (" +
                      track_in +
                      ") ORDER BY ta.track_id, ta.position, ta.artist_id",
                  track_ids)
           .rows) {
    track_artists[static_cast<int>(int_at(row, 0))].push_back(ArtistLink{
        ref_for(EntityType::Artist, static_cast<int>(int_at(row, 1))),
        text_at(row, 2)});
  }

  std::unordered_set<int> with_lyrics;
  for (const db::Row &row :
       db()
           .query("SELECT track_id FROM track_lyrics WHERE track_id IN (" +
                      track_in + ") AND text <> ''",
                  track_ids)
           .rows) {
    with_lyrics.insert(static_cast<int>(int_at(row, 0)));
  }

  std::unordered_map<int, repo::Album> albums;
  std::unordered_map<int, std::vector<AlbumArtistLink>> album_artists;
  if (!album_ids.empty()) {
    repo::AlbumRepo album_repo(db());
    for (repo::Album &album : album_repo.byIds(album_ids)) {
      albums.emplace(album.id, std::move(album));
    }

    db::ParamList album_params;
    album_params.reserve(album_ids.size());
    for (const int album_id : album_ids) {
      album_params.push_back(static_cast<std::int64_t>(album_id));
    }
    for (const db::Row &row :
         db()
             .query("SELECT aa.album_id, a.id, a.name FROM album_artist aa "
                    "JOIN artist a ON a.id = aa.artist_id WHERE aa.album_id "
                    "IN (" +
                        schema::sql::placeholders(album_ids.size()) +
                        ") ORDER BY aa.album_id, aa.position, aa.artist_id",
                    album_params)
             .rows) {
      album_artists[static_cast<int>(int_at(row, 0))].push_back(
          AlbumArtistLink{
              ref_for(EntityType::AlbumArtist,
                      static_cast<int>(int_at(row, 1))),
              text_at(row, 2)});
    }
  }

  for (const repo::Track &row : rows) {
    Track track;
    track.ref = ref_for(EntityType::Track, row.id);
    track.title = row.title;
    track.duration_ms = row.duration_ms;
    track.track_no = row.track_no;
    track.disc_no = row.disc_no;
    const auto genre_it = genres.find(row.id);
    if (genre_it != genres.end()) {
      track.genres = genre_it->second;
    }
    track.user_data.play_count = row.play_count;
    track.user_data.last_played = row.last_played;
    track.user_data.rating = row.rating;
    track.user_data.added_at = row.added_at;
    track.audio.bitrate_kbps = row.bitrate;
    track.audio.sample_rate = row.sample_rate;
    track.audio.channels = row.channels;
    track.audio.format = row.format;
    track.audio.size_bytes = row.size_bytes;

    const auto artist_it = track_artists.find(row.id);
    if (artist_it != track_artists.end()) {
      track.artists = artist_it->second;
    }

    const auto album_it = albums.find(row.album_id);
    if (album_it != albums.end()) {
      const repo::Album &album = album_it->second;
      track.album =
          AlbumLink{ref_for(EntityType::Album, album.id), album.name};
      track.year = album.year;
      const auto album_artist_it = album_artists.find(album.id);
      if (album_artist_it != album_artists.end()) {
        track.album_artists = album_artist_it->second;
      }
      if (!album.cover_path.empty()) {
        track.artwork = ArtworkRef{
            ref_for(EntityType::Album, album.id), "primary", std::nullopt,
            std::to_string(std::hash<std::string>{}(album.cover_path))};
      }
    }

    track.lyrics_available = with_lyrics.count(row.id) != 0;
    out.push_back(std::move(track));
  }
  return out;
}

std::vector<Album>
LocalLibrary::build_albums(const std::vector<repo::Album> &rows) {
  std::vector<Album> out;
  if (rows.empty()) {
    return out;
  }
  out.reserve(rows.size());

  db::ParamList album_params;
  album_params.reserve(rows.size());
  for (const repo::Album &row : rows) {
    album_params.push_back(static_cast<std::int64_t>(row.id));
  }

  std::unordered_map<int, std::vector<AlbumArtistLink>> album_artists;
  for (const db::Row &row :
       db()
           .query("SELECT aa.album_id, a.id, a.name FROM album_artist aa "
                  "JOIN artist a ON a.id = aa.artist_id WHERE aa.album_id "
                  "IN (" +
                      schema::sql::placeholders(rows.size()) +
                      ") ORDER BY aa.album_id, aa.position, aa.artist_id",
                  album_params)
           .rows) {
    album_artists[static_cast<int>(int_at(row, 0))].push_back(
        AlbumArtistLink{
            ref_for(EntityType::AlbumArtist, static_cast<int>(int_at(row, 1))),
            text_at(row, 2)});
  }

  for (const repo::Album &row : rows) {
    Album album;
    album.ref = ref_for(EntityType::Album, row.id);
    album.title = row.name;
    album.year = row.year;
    const auto artist_it = album_artists.find(row.id);
    if (artist_it != album_artists.end()) {
      album.artists = artist_it->second;
    }
    if (!row.cover_path.empty()) {
      album.artwork = ArtworkRef{
          album.ref, "primary", std::nullopt,
          std::to_string(std::hash<std::string>{}(row.cover_path))};
    }
    out.push_back(std::move(album));
  }
  return out;
}

std::vector<Artist>
LocalLibrary::build_artists(const std::vector<repo::Artist> &rows) {
  std::vector<Artist> out;
  if (rows.empty()) {
    return out;
  }
  out.reserve(rows.size());

  db::ParamList artist_params;
  artist_params.reserve(rows.size());
  for (const repo::Artist &row : rows) {
    artist_params.push_back(static_cast<std::int64_t>(row.id));
  }

  // 与单条 cover_path_of 的语义一致：任一有封面的专辑即可；
  // 批量路径固定取 album id 最小的一张，避免结果随扫描顺序变化。
  std::unordered_map<int, std::string> covers;
  for (const db::Row &row :
       db()
           .query("SELECT aa.artist_id, al.cover_path FROM album_artist aa "
                  "JOIN album al ON al.id = aa.album_id WHERE aa.artist_id "
                  "IN (" +
                      schema::sql::placeholders(rows.size()) +
                      ") AND al.cover_path IS NOT NULL AND al.cover_path <> '' "
                      "ORDER BY aa.artist_id, al.id",
                  artist_params)
           .rows) {
    covers.emplace(static_cast<int>(int_at(row, 0)), text_at(row, 1));
  }

  for (const repo::Artist &row : rows) {
    Artist artist;
    artist.ref = ref_for(EntityType::Artist, row.id);
    artist.name = row.name;
    artist.sort_name = row.sort_name;
    const auto cover_it = covers.find(row.id);
    if (cover_it != covers.end()) {
      artist.artwork = ArtworkRef{
          artist.ref, "primary", std::nullopt,
          std::to_string(std::hash<std::string>{}(cover_it->second))};
    }
    out.push_back(std::move(artist));
  }
  return out;
}

Result<Track> LocalLibrary::get_track(const TrackRef &ref,
                                      const CallContext &) {
  if (ref.entity_type != EntityType::Track || ref.library_id != library_id_) {
    return Result<Track>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid track reference"));
  }
  const auto internal_id = internal_id_of(ref);
  if (!internal_id) {
    return Result<Track>::failure(
        Error::make(ErrorCode::NotFound, "track not found"));
  }
  repo::TrackRepo tracks(db());
  const auto row = tracks.byId(*internal_id);
  if (!row) {
    return Result<Track>::failure(
        Error::make(ErrorCode::NotFound, "track not found"));
  }
  return Result<Track>::success(build_track(*row));
}

Result<Album> LocalLibrary::get_album(const AlbumRef &ref,
                                      const CallContext &) {
  if (ref.entity_type != EntityType::Album || ref.library_id != library_id_) {
    return Result<Album>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid album reference"));
  }
  const auto internal_id = internal_id_of(ref);
  if (!internal_id) {
    return Result<Album>::failure(
        Error::make(ErrorCode::NotFound, "album not found"));
  }
  repo::AlbumRepo albums(db());
  const auto row = albums.byId(*internal_id);
  if (!row) {
    return Result<Album>::failure(
        Error::make(ErrorCode::NotFound, "album not found"));
  }
  return Result<Album>::success(build_album(*row));
}

Result<Artist> LocalLibrary::get_artist(const ArtistRef &ref,
                                        const CallContext &) {
  if (ref.entity_type != EntityType::Artist || ref.library_id != library_id_) {
    return Result<Artist>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid artist reference"));
  }
  const auto internal_id = internal_id_of(ref);
  if (!internal_id) {
    return Result<Artist>::failure(
        Error::make(ErrorCode::NotFound, "artist not found"));
  }
  repo::ArtistRepo artists(db());
  const auto row = artists.byId(*internal_id);
  if (!row) {
    return Result<Artist>::failure(
        Error::make(ErrorCode::NotFound, "artist not found"));
  }
  return Result<Artist>::success(build_artist(*row));
}

Result<Page<Track>> LocalLibrary::list_album_tracks(const AlbumRef &ref,
                                                    const PageRequest &page,
                                                    const CallContext &) {
  if (ref.entity_type != EntityType::Album || ref.library_id != library_id_) {
    return Result<Page<Track>>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid album reference"));
  }
  const auto internal_id = internal_id_of(ref);
  if (!internal_id) {
    return Result<Page<Track>>::failure(
        Error::make(ErrorCode::NotFound, "album not found"));
  }
  repo::TrackRepo tracks(db());
  std::vector<repo::Track> rows = tracks.byAlbum(*internal_id);
  Page<Track> out;
  const int limit = page.limit > 0 ? page.limit : 50;
  const std::size_t total = rows.size();
  const std::size_t take =
      std::min<std::size_t>(total, static_cast<std::size_t>(limit));
  rows.resize(take);

  // 详情页一次取整张专辑：曲目行已由 byAlbum 取回，其余关联数据由
  // build_tracks 按 id 集合批量查，避免每首曲目再发多条 SQL（远端
  // PostgreSQL 下 15 首就是 100+ 次往返）。
  out.items = build_tracks(rows);
  out.has_more = total > out.items.size();
  return Result<Page<Track>>::success(std::move(out));
}

Result<Page<Album>> LocalLibrary::list_artist_albums(const ArtistRef &ref,
                                                     const PageRequest &page,
                                                     const CallContext &) {
  if (ref.entity_type != EntityType::Artist || ref.library_id != library_id_) {
    return Result<Page<Album>>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid artist reference"));
  }
  const auto internal_id = internal_id_of(ref);
  if (!internal_id) {
    return Result<Page<Album>>::failure(
        Error::make(ErrorCode::NotFound, "artist not found"));
  }
  repo::AlbumRepo albums(db());
  std::vector<repo::Album> rows = albums.byArtist(*internal_id);
  Page<Album> out;
  const int limit = page.limit > 0 ? page.limit : 50;
  const std::size_t total = rows.size();
  const std::size_t take =
      std::min<std::size_t>(total, static_cast<std::size_t>(limit));
  rows.resize(take);

  // 专辑行已带 artist_ids 聚合列，这里只批量补艺术家名。
  out.items = build_albums(rows);
  out.has_more = total > out.items.size();
  return Result<Page<Album>>::success(std::move(out));
}

Result<Page<SearchHit>> LocalLibrary::search(const SearchQuery &query,
                                             const CallContext &) {
  // types 决定出哪些实体：客户端「专辑」页要 Album，不能再拿曲目顶替
  // （否则卡片是 Audio，点进去按专辑取子项必然为空）。
  const bool all_types = query.types.empty();
  const auto wants = [&query, all_types](EntityType type) {
    return all_types || std::find(query.types.begin(), query.types.end(),
                                  type) != query.types.end();
  };
  const bool want_tracks = wants(EntityType::Track);
  const bool want_albums = wants(EntityType::Album);
  const bool want_artists =
      wants(EntityType::Artist) || wants(EntityType::AlbumArtist);

  Page<SearchHit> page;
  if (query.text.empty() || (!want_tracks && !want_albums && !want_artists)) {
    return Result<Page<SearchHit>>::success(std::move(page));
  }

  const int limit = query.page.limit > 0 ? query.page.limit : 50;
  // 三类命中按「专辑 -> 艺术家 -> 曲目」的固定顺序拼成一条结果流，所以
  // 每类各取到 offset + limit + 1 条就够切片；窗口设上限，避免深偏移把
  // 一次请求放大成整表扫描。
  constexpr int kMaxWindow = 2000;
  const int offset = search_offset(query.page.cursor);
  if (offset >= kMaxWindow) {
    return Result<Page<SearchHit>>::success(std::move(page));
  }
  const int want = std::min(offset + limit + 1, kMaxWindow);
  std::vector<SearchHit> hits;

  if (want_albums) {
    repo::AlbumRepo albums(db());
    for (Album &album : build_albums(albums.search(query.text, want))) {
      SearchHit hit;
      hit.kind = EntityType::Album;
      hit.ref = album.ref;
      hit.title = album.title;
      hit.album_artists = album.artists;
      if (!album.artists.empty()) {
        hit.subtitle = album.artists.front().name;
      }
      hit.artwork = album.artwork;
      hits.push_back(std::move(hit));
    }
  }

  if (want_artists) {
    repo::ArtistRepo artists(db());
    for (Artist &artist : build_artists(artists.search(query.text, want))) {
      SearchHit hit;
      hit.kind = EntityType::Artist;
      hit.ref = artist.ref;
      hit.title = artist.name;
      hit.artwork = artist.artwork;
      hits.push_back(std::move(hit));
    }
  }

  if (want_tracks) {
    repo::TrackRepo track_repo(db());
    std::vector<Track> tracks =
        build_tracks(track_repo.search(query.text, want));
    for (Track &track : tracks) {
      if (!query.artists.empty()) {
        const bool match =
            std::any_of(query.artists.begin(), query.artists.end(),
                        [&](const std::string &name) {
                          return contains_name(track.artists, name);
                        });
        if (!match) {
          continue;
        }
      }
      if (!query.album_artists.empty()) {
        const bool match =
            std::any_of(query.album_artists.begin(), query.album_artists.end(),
                        [&](const std::string &name) {
                          return contains_name(track.album_artists, name);
                        });
        if (!match) {
          continue;
        }
      }

      SearchHit hit;
      hit.kind = EntityType::Track;
      hit.ref = track.ref;
      hit.title = track.title;
      hit.artists = track.artists;
      hit.album_artists = track.album_artists;
      hit.album = track.album;
      hit.artwork = track.artwork;
      hit.duration_ms = track.duration_ms;
      hit.playable = track.playable;
      if (!track.artists.empty()) {
        hit.subtitle = track.artists.front().name;
      }
      hits.push_back(std::move(hit));
    }
  }

  const int total = static_cast<int>(hits.size());
  const int start = std::min(offset, total);
  const int take = std::min(limit, total - start);
  page.items.reserve(static_cast<std::size_t>(take));
  for (int i = 0; i < take; ++i) {
    page.items.push_back(std::move(hits[static_cast<std::size_t>(start + i)]));
  }
  page.has_more = total > start + take;
  if (page.has_more) {
    page.next_cursor = std::to_string(start + take);
  }
  return Result<Page<SearchHit>>::success(std::move(page));
}

Result<PlaybackSource> LocalLibrary::resolve_playback(const TrackRef &ref,
                                                      const CallContext &) {
  if (ref.entity_type != EntityType::Track || ref.library_id != library_id_) {
    return Result<PlaybackSource>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid track reference"));
  }
  const auto internal_id = internal_id_of(ref);
  if (!internal_id) {
    return Result<PlaybackSource>::failure(
        Error::make(ErrorCode::NotFound, "track not found"));
  }
  repo::TrackRepo tracks(db());
  const auto row = tracks.byId(*internal_id);
  if (!row) {
    return Result<PlaybackSource>::failure(
        Error::make(ErrorCode::NotFound, "track not found"));
  }

  PlaybackSource source;
  source.track = ref;
  // 本地库的扫描根可以包含 WebDAV 根：kind 必须描述真实字节来源，
  // 否则下游会把远端 URL 当成本地文件去打开。
  source.internal_locator = storage::resolve(row->path);
  source.kind = webdav::isWebdavUrl(source.internal_locator)
                    ? PlaybackKind::WebDav
                    : PlaybackKind::LocalFile;
  source.mime_type = mime_for(row->format);
  source.content_length = row->size_bytes;
  source.range_supported = true;
  source.cacheable = false;
  source.access_mode = AccessMode::Proxy;
  return Result<PlaybackSource>::success(std::move(source));
}

Result<Lyrics> LocalLibrary::get_lyrics(const TrackRef &ref,
                                        const CallContext &) {
  if (ref.entity_type != EntityType::Track || ref.library_id != library_id_) {
    return Result<Lyrics>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid track reference"));
  }
  const auto internal_id = internal_id_of(ref);
  if (!internal_id) {
    return Result<Lyrics>::failure(
        Error::make(ErrorCode::NotFound, "track not found"));
  }

  std::string text;
  {
    const auto result = db().query(schema::sql::kTrackLyricsByTrackId,
                                   {static_cast<std::int64_t>(*internal_id)});
    if (result.rows.empty()) {
      return Result<Lyrics>::failure(
          Error::make(ErrorCode::NotFound, "lyrics not found"));
    }
    text = text_at(result.rows.front(), 0);
  }
  if (text.empty()) {
    return Result<Lyrics>::failure(
        Error::make(ErrorCode::NotFound, "lyrics not found"));
  }

  // 解析统一在媒体库层完成（与远端同一份实现）。
  Lyrics lyrics = parse_plain_lyrics(ref, text);
  return Result<Lyrics>::success(std::move(lyrics));
}

std::string LocalLibrary::cover_path_of(const SourceRef &owner) const {
  const auto internal_id = internal_id_of(owner);
  if (!internal_id) {
    return {};
  }

  if (owner.entity_type == EntityType::Album) {
    const auto result = db().query("SELECT cover_path FROM album WHERE id = ?",
                                   {static_cast<std::int64_t>(*internal_id)});
    if (!result.rows.empty() && !is_null_at(result.rows.front(), 0)) {
      return text_at(result.rows.front(), 0);
    }
    return {};
  }

  if (owner.entity_type == EntityType::Track) {
    const auto result = db().query(
        "SELECT al.cover_path FROM track t JOIN album al ON al.id = t.album_id "
        "WHERE t.id = ?",
        {static_cast<std::int64_t>(*internal_id)});
    if (!result.rows.empty() && !is_null_at(result.rows.front(), 0)) {
      return text_at(result.rows.front(), 0);
    }
    return {};
  }

  if (owner.entity_type == EntityType::Artist ||
      owner.entity_type == EntityType::AlbumArtist) {
    const auto result = db().query(
        "SELECT al.cover_path FROM album al JOIN album_artist aa ON "
        "aa.album_id = al.id WHERE aa.artist_id = ? AND al.cover_path IS NOT "
        "NULL AND al.cover_path <> '' LIMIT 1",
        {static_cast<std::int64_t>(*internal_id)});
    if (!result.rows.empty()) {
      return text_at(result.rows.front(), 0);
    }
  }
  return {};
}

std::optional<std::string>
LocalLibrary::artwork_path(const SourceRef &owner) const {
  const std::string path = cover_path_of(owner);
  if (path.empty()) {
    return std::nullopt;
  }
  return path;
}

Result<ArtworkRef> LocalLibrary::get_artwork(const SourceRef &owner,
                                             const CallContext &) {
  const std::string path = cover_path_of(owner);
  if (path.empty()) {
    return Result<ArtworkRef>::failure(
        Error::make(ErrorCode::NotFound, "artwork not found"));
  }
  ArtworkRef artwork;
  artwork.owner = owner;
  artwork.kind = "primary";
  artwork.cache_tag = std::to_string(std::hash<std::string>{}(path));
  return Result<ArtworkRef>::success(std::move(artwork));
}

Result<ArtworkLocation> LocalLibrary::artwork_location(const ArtworkRef &ref,
                                                       const CallContext &) {
  const std::string path = cover_path_of(ref.owner);
  if (path.empty()) {
    return Result<ArtworkLocation>::failure(
        Error::make(ErrorCode::NotFound, "artwork not found"));
  }

  ArtworkLocation location;
  location.mime_type = http_util::mimeType(std::filesystem::path(path));
  // 虚拟路径原样带出：ArtworkService 用它命中 image_cache；
  // locator 是解析后的真实路径，只在本进程内流转。
  location.cache_virtual_path = path;
  location.locator = storage::resolve(path);
  return Result<ArtworkLocation>::success(std::move(location));
}

CatalogSnapshot LocalLibrary::snapshot() {
  CatalogSnapshot out;

  std::unordered_map<int, std::string> artist_names;
  {
    const auto result = db().query("SELECT id, name FROM artist");
    for (const auto &row : result.rows) {
      artist_names.emplace(static_cast<int>(int_at(row, 0)), text_at(row, 1));
    }
  }

  std::unordered_map<int, std::vector<int>> album_artist_ids;
  {
    const auto result =
        db().query("SELECT album_id, artist_id FROM album_artist ORDER BY "
                   "album_id, position");
    for (const auto &row : result.rows) {
      album_artist_ids[static_cast<int>(int_at(row, 0))].push_back(
          static_cast<int>(int_at(row, 1)));
    }
  }

  std::unordered_map<int, std::vector<int>> track_artist_ids;
  {
    const auto result =
        db().query("SELECT track_id, artist_id FROM track_artist ORDER BY "
                   "track_id, position");
    for (const auto &row : result.rows) {
      track_artist_ids[static_cast<int>(int_at(row, 0))].push_back(
          static_cast<int>(int_at(row, 1)));
    }
  }

  std::unordered_map<int, std::vector<GenreEntry>> genre_by_track;
  std::unordered_set<std::string> seen_genres;
  {
    const auto result =
        db().query("SELECT tg.track_id, g.id, g.name FROM track_genre tg "
                   "JOIN genre g ON g.id = tg.genre_id ORDER BY g.name");
    for (const auto &row : result.rows) {
      const GenreEntry entry{static_cast<int>(int_at(row, 1)), text_at(row, 2)};
      genre_by_track[static_cast<int>(int_at(row, 0))].push_back(entry);
      if (seen_genres.insert(entry.name).second) {
        out.genres.push_back(entry);
      }
    }
  }

  std::unordered_map<int, long long> album_duration;
  std::unordered_map<int, int> album_count;
  std::unordered_map<int, std::size_t> album_index_by_internal;
  std::unordered_map<int, std::string> album_cover_tag;
  {
    const auto result = db().query(
        "SELECT album_id, COUNT(*), COALESCE(SUM(duration_ms), 0) FROM track "
        "WHERE album_id > 0 GROUP BY album_id");
    for (const auto &row : result.rows) {
      const int album_id = static_cast<int>(int_at(row, 0));
      album_count[album_id] = static_cast<int>(int_at(row, 1));
      album_duration[album_id] = int_at(row, 2);
    }
  }

  std::unordered_set<int> lyrics_tracks;
  {
    const auto result =
        db().query("SELECT track_id FROM track_lyrics WHERE text <> ''");
    for (const auto &row : result.rows) {
      lyrics_tracks.insert(static_cast<int>(int_at(row, 0)));
    }
  }

  {
    const auto result = db().query("SELECT a.id, a.name, a.year, a.cover_path "
                                   "FROM album a ORDER BY a.name");
    for (const auto &source : result.rows) {
      repo::Album row;
      row.id = static_cast<int>(int_at(source, 0));
      row.name = text_at(source, 1);
      row.year = static_cast<int>(int_at(source, 2));
      row.cover_path =
          is_null_at(source, 3) ? std::string() : text_at(source, 3);
      const auto it = album_artist_ids.find(row.id);
      if (it != album_artist_ids.end()) {
        row.artist_ids = it->second;
      }

      Album album;
      album.ref = ref_for(EntityType::Album, row.id);
      album.title = row.name;
      album.year = row.year > 0 ? std::optional<int>(row.year) : std::nullopt;
      for (const int artist_id : row.artist_ids) {
        const auto name = artist_names.find(artist_id);
        if (name == artist_names.end()) {
          continue;
        }
        album.artists.push_back(AlbumArtistLink{
            ref_for(EntityType::AlbumArtist, artist_id), name->second});
      }
      if (!row.cover_path.empty()) {
        album.artwork = ArtworkRef{
            ref_for(EntityType::Album, row.id), "primary", std::nullopt,
            std::to_string(std::hash<std::string>{}(row.cover_path))};
        album_cover_tag[row.id] = album.artwork->cache_tag;
      }
      out.album_track_count[row.id] = album_count[row.id];
      out.album_duration_ms[row.id] = album_duration[row.id];
      out.artist_album_count[row.id] = 0;
      album_index_by_internal[row.id] = out.albums.size();
      out.album_internal_ids.push_back(row.id);
      out.albums.push_back(std::move(album));
    }
  }

  {
    const auto result =
        db().query("SELECT id, name, sort_name FROM artist ORDER BY "
                   "COALESCE(NULLIF(sort_name, ''), name)");
    for (const auto &row : result.rows) {
      Artist artist;
      artist.ref =
          ref_for(EntityType::Artist, static_cast<int>(int_at(row, 0)));
      artist.name = text_at(row, 1);
      artist.sort_name = is_null_at(row, 2) ? std::string() : text_at(row, 2);
      out.artist_internal_ids.push_back(static_cast<int>(int_at(row, 0)));
      out.artists.push_back(std::move(artist));
    }
  }

  for (const auto &kv : album_artist_ids) {
    for (const int artist_id : kv.second) {
      out.artist_album_count[artist_id] += 1;
    }
  }

  {
    const auto result = db().query(
        "SELECT id, root_id, path, title, album_id, disc_no, track_no, "
        "duration_ms, bitrate, sample_rate, channels, format, size_bytes, "
        "mtime, "
        "added_at, play_count, last_played, rating FROM track "
        "ORDER BY album_id, disc_no, track_no, id");
    for (const auto &source : result.rows) {
      repo::Track row;
      row.id = static_cast<int>(int_at(source, 0));
      row.root_id = static_cast<int>(int_at(source, 1));
      row.path = is_null_at(source, 2) ? std::string() : text_at(source, 2);
      row.title = is_null_at(source, 3) ? std::string() : text_at(source, 3);
      row.album_id = static_cast<int>(int_at(source, 4));
      row.disc_no = static_cast<int>(int_at(source, 5));
      row.track_no = static_cast<int>(int_at(source, 6));
      row.duration_ms = static_cast<int>(int_at(source, 7));
      row.bitrate = static_cast<int>(int_at(source, 8));
      row.sample_rate = static_cast<int>(int_at(source, 9));
      row.channels = static_cast<int>(int_at(source, 10));
      row.format = is_null_at(source, 11) ? std::string() : text_at(source, 11);
      row.size_bytes = int_at(source, 12);
      row.mtime = int_at(source, 13);
      row.added_at =
          is_null_at(source, 14) ? std::string() : text_at(source, 14);
      row.play_count = static_cast<int>(int_at(source, 15));
      row.last_played =
          is_null_at(source, 16) ? std::string() : text_at(source, 16);
      row.rating = static_cast<int>(int_at(source, 17));

      const auto tracks_it = track_artist_ids.find(row.id);
      if (tracks_it != track_artist_ids.end()) {
        row.artist_ids = tracks_it->second;
      }

      Track track;
      track.ref = ref_for(EntityType::Track, row.id);
      track.title = row.title;
      track.duration_ms = row.duration_ms > 0
                              ? std::optional<long long>(row.duration_ms)
                              : std::nullopt;
      track.track_no =
          row.track_no > 0 ? std::optional<int>(row.track_no) : std::nullopt;
      track.disc_no =
          row.disc_no > 0 ? std::optional<int>(row.disc_no) : std::nullopt;
      track.user_data.play_count = row.play_count;
      track.user_data.last_played = row.last_played;
      track.user_data.rating = row.rating;
      track.user_data.added_at = row.added_at;
      track.audio.bitrate_kbps =
          row.bitrate > 0 ? std::optional<int>(row.bitrate) : std::nullopt;
      track.audio.sample_rate = row.sample_rate > 0
                                    ? std::optional<int>(row.sample_rate)
                                    : std::nullopt;
      track.audio.channels =
          row.channels > 0 ? std::optional<int>(row.channels) : std::nullopt;
      track.audio.format = row.format;
      track.audio.size_bytes = row.size_bytes > 0
                                   ? std::optional<long long>(row.size_bytes)
                                   : std::nullopt;

      for (const int artist_id : row.artist_ids) {
        const auto name = artist_names.find(artist_id);
        if (name == artist_names.end()) {
          continue;
        }
        track.artists.push_back(
            ArtistLink{ref_for(EntityType::Artist, artist_id), name->second});
      }

      if (row.album_id > 0) {
        const auto album_it = album_artist_ids.find(row.album_id);
        const auto album_pos = album_index_by_internal.find(row.album_id);
        if (album_pos != album_index_by_internal.end()) {
          const Album &album = out.albums[album_pos->second];
          track.album = AlbumLink{album.ref, album.title};
          track.year = album.year;
        }
        if (album_it != album_artist_ids.end()) {
          for (const int artist_id : album_it->second) {
            const auto name = artist_names.find(artist_id);
            if (name == artist_names.end()) {
              continue;
            }
            track.album_artists.push_back(AlbumArtistLink{
                ref_for(EntityType::AlbumArtist, artist_id), name->second});
          }
        }
        if (const auto cover = album_cover_tag.find(row.album_id);
            cover != album_cover_tag.end()) {
          track.artwork = ArtworkRef{ref_for(EntityType::Album, row.album_id),
                                     "primary", std::nullopt, cover->second};
        }
      }

      const auto genres_it = genre_by_track.find(row.id);
      if (genres_it != genre_by_track.end()) {
        for (const auto &genre : genres_it->second) {
          track.genres.push_back(genre.name);
        }
        out.track_genres[row.id] = genres_it->second;
      }

      track.lyrics_available = lyrics_tracks.count(row.id) != 0;
      out.track_paths[row.id] = row.path;
      out.track_internal_ids.push_back(row.id);
      out.tracks.push_back(std::move(track));
    }
  }

  {
    const auto result = db().query(
        "SELECT t.id FROM track t JOIN play_event pe ON t.id = pe.track_id "
        "ORDER BY pe.played_at DESC");
    for (const auto &row : result.rows) {
      out.recent_play_track_ids.push_back(static_cast<int>(int_at(row, 0)));
    }
  }

  return out;
}

std::optional<TrackTransport>
LocalLibrary::track_transport(int internal_id) const {
  TrackTransport transport;
  {
    const auto result =
        db().query("SELECT path, mtime, album_id FROM track WHERE id = ?",
                   {static_cast<std::int64_t>(internal_id)});
    if (!result.rows.empty()) {
      const auto &row = result.rows.front();
      transport.path = is_null_at(row, 0) ? std::string() : text_at(row, 0);
      transport.mtime = int_at(row, 1);
      transport.album_internal_id = static_cast<int>(int_at(row, 2));
    } else {
      return std::nullopt;
    }
  }
  {
    const auto result =
        db().query("SELECT artist_id FROM track_artist WHERE track_id = ? "
                   "ORDER BY position",
                   {static_cast<std::int64_t>(internal_id)});
    for (const auto &row : result.rows) {
      transport.artist_internal_ids.push_back(static_cast<int>(int_at(row, 0)));
    }
  }
  return transport;
}

// ---------------------------------------------------------------------------
// CatalogLister：门面的聚合列表只经由本接口取全量条目，不再遍历库。
// 实现走本地快照（一次读全，内部再用 SQL 的收益远小于一致性成本）。
// ---------------------------------------------------------------------------

namespace {

std::int64_t local_offset_of(const std::optional<std::string> &cursor) {
  if (!cursor.has_value() || cursor->empty()) {
    return 0;
  }
  try {
    const long long parsed = std::stoll(*cursor);
    return parsed > 0 ? static_cast<std::int64_t>(parsed) : 0;
  } catch (const std::exception &) {
    return 0;
  }
}

template <typename T>
Page<T> local_slice_page(const std::vector<T> &all, const PageRequest &page) {
  const std::int64_t total = static_cast<std::int64_t>(all.size());
  const std::int64_t offset = std::min(local_offset_of(page.cursor), total);
  std::int64_t end = total;
  if (page.limit > 0) {
    end = std::min(total, offset + static_cast<std::int64_t>(page.limit));
  }
  Page<T> out;
  out.items.reserve(static_cast<std::size_t>(end - offset));
  for (std::int64_t i = offset; i < end; ++i) {
    out.items.push_back(all[static_cast<std::size_t>(i)]);
  }
  if (end < total) {
    out.has_more = true;
    out.next_cursor = std::to_string(end);
  }
  return out;
}

} // namespace

Result<Page<Track>> LocalLibrary::list_tracks(const PageRequest &page,
                                              const CallContext &) {
  const CatalogSnapshot snap = snapshot();
  return Result<Page<Track>>::success(local_slice_page(snap.tracks, page));
}

Result<Page<Album>> LocalLibrary::list_albums(const PageRequest &page,
                                              const CallContext &) {
  const CatalogSnapshot snap = snapshot();
  return Result<Page<Album>>::success(local_slice_page(snap.albums, page));
}

Result<Page<Artist>> LocalLibrary::list_artists(const PageRequest &page,
                                                const CallContext &) {
  const CatalogSnapshot snap = snapshot();
  return Result<Page<Artist>>::success(local_slice_page(snap.artists, page));
}

Result<Page<Track>> LocalLibrary::list_recent(const PageRequest &page,
                                              const CallContext &) {
  CatalogSnapshot snap = snapshot();
  std::stable_sort(snap.tracks.begin(), snap.tracks.end(),
                   [](const Track &a, const Track &b) {
                     return a.user_data.added_at > b.user_data.added_at;
                   });
  return Result<Page<Track>>::success(local_slice_page(snap.tracks, page));
}

// ---------------------------------------------------------------------------
// 本地歌单：把 `./playlists` 下的 JSON 歌单包成库能力，让播放列表总管能跨库
// 汇总。这块数据不在数据库里，曲目靠「虚拟路径」在启动时匹配成 track_id。
// ---------------------------------------------------------------------------

namespace {

// 本地歌单的 ref：entity_id 就是歌单文件的 32 位十六进制 id。
PlaylistRef local_playlist_ref(const std::string &library_id,
                               const std::string &playlist_id) {
  PlaylistRef ref;
  ref.library_id = library_id;
  ref.entity_type = EntityType::Playlist;
  ref.entity_id = playlist_id;
  return ref;
}

} // namespace

Result<Page<Playlist>> LocalLibrary::list_playlists(const PageRequest &page,
                                                    const CallContext &) {
  // 列表只给摘要：曲目留给 get_playlist 展开，否则列一次歌单就要把每个歌单
  // 的每首歌都读一遍。
  const std::vector<playlist::Playlist> stored = playlist::list();
  std::vector<Playlist> summaries;
  summaries.reserve(stored.size());
  for (const playlist::Playlist &item : stored) {
    Playlist summary;
    summary.ref = local_playlist_ref(library_id_, item.id);
    summary.name = item.name;
    summaries.push_back(std::move(summary));
  }
  // 游标语义与本地 CatalogLister 一致：十进制 offset。
  return Result<Page<Playlist>>::success(local_slice_page(summaries, page));
}

Result<Playlist> LocalLibrary::get_playlist(const PlaylistRef &ref,
                                            const CallContext &ctx) {
  using R = Result<Playlist>;
  if (ref.entity_type != EntityType::Playlist ||
      ref.library_id != library_id_) {
    return R::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid playlist reference"));
  }

  playlist::Playlist stored;
  if (!playlist::get(ref.entity_id, stored)) {
    return R::failure(
        Error::make(ErrorCode::NotFound, "local playlist not found"));
  }

  Playlist out;
  out.ref = ref;
  out.name = stored.name;
  out.tracks.reserve(stored.songs.size());
  for (const playlist::Song &song : stored.songs) {
    // 扫库后路径不在了：丢弃，只出能真正播放的曲目。
    if (!song.matched || song.track_id <= 0) {
      continue;
    }
    const SourceRef track_ref = ref_for(EntityType::Track, song.track_id);
    if (track_ref.entity_id.empty()) {
      continue;
    }
    Result<Track> track = get_track(track_ref, ctx);
    if (!track.ok()) {
      continue;
    }
    out.tracks.push_back(std::move(track.value()));
  }
  return R::success(std::move(out));
}

db::Session &LocalLibrary::db() const { return db::mainSession(); }

} // namespace media_library
