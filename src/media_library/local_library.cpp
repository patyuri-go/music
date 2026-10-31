#include "media_library/local_library.h"

#include "config.h"
#include "db.h"
#include "http_util.h"
#include "schema.h"
#include "storage.h"

#include "api/playlist/playlist.h"
#include "api/webdav/webdav.h"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <optional>
#include <unordered_set>

namespace media_library {
namespace {

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

std::optional<long long> parse_timestamp(const std::string &text) {
  std::vector<std::string> parts;
  std::string current;
  for (const char ch : text) {
    if (ch == ':') {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  parts.push_back(current);
  if (parts.size() < 2 || parts.size() > 3) {
    return std::nullopt;
  }

  long long fraction_ms = 0;
  std::string &last = parts.back();
  const std::size_t dot = last.find_first_of(".,");
  if (dot != std::string::npos) {
    std::string digits;
    for (std::size_t i = dot + 1; i < last.size(); ++i) {
      if (std::isdigit(static_cast<unsigned char>(last[i])) != 0) {
        digits.push_back(last[i]);
      }
    }
    if (!digits.empty()) {
      fraction_ms =
          static_cast<long long>(std::atof(("0." + digits).c_str()) * 1000.0);
    }
    last = last.substr(0, dot);
  }

  long long total = 0;
  for (const auto &part : parts) {
    if (part.empty()) {
      return std::nullopt;
    }
    for (const char ch : part) {
      if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
        return std::nullopt;
      }
    }
    total = total * 60 + std::stoll(part);
  }
  return total * 1000 + fraction_ms;
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
  return std::any_of(links.begin(), links.end(),
                     [&](const AlbumArtistLink &link) {
                       return lower(link.name) == needle;
                     });
}

} // namespace

LocalLibrary::LocalLibrary(std::string library_id, std::string display_name,
                           std::string db_path)
    : library_id_(std::move(library_id)), display_name_(std::move(display_name)),
      db_path_(std::move(db_path)),
      entities_(library_id_, db_path_) {}

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

  entities_.backfill();
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
  return entities_.ref_for(type, internal_id);
}

std::optional<SourceRef> LocalLibrary::ref_for_compat(EntityType type,
                                                      int internal_id) {
  if (internal_id <= 0) {
    return std::nullopt;
  }
  return entities_.ref_for(type, internal_id);
}

std::optional<int> LocalLibrary::internal_id_of(const SourceRef &ref) const {
  return entities_.internal_id_of(ref);
}

std::vector<std::string> LocalLibrary::genres_of(int track_id) const {
  SQLite::Database &database = db::conn(db_path_);
  SQLite::Statement query(
      database,
      "SELECT g.name FROM track_genre tg JOIN genre g ON g.id = tg.genre_id "
      "WHERE tg.track_id = ? ORDER BY g.name");
  query.bind(1, track_id);
  std::vector<std::string> out;
  while (query.executeStep()) {
    out.push_back(query.getColumn(0).getText());
  }
  return out;
}

std::optional<repo::Album> LocalLibrary::album_of(const repo::Track &row) const {
  if (row.album_id <= 0) {
    return std::nullopt;
  }
  repo::AlbumRepo albums(db_path_);
  return albums.byId(row.album_id);
}

Track LocalLibrary::build_track(const repo::Track &row) {
  Track track;
  track.ref = entities_.ref_for(EntityType::Track, row.id);
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

  repo::ArtistRepo artists(db_path_);
  for (const int artist_id : row.artist_ids) {
    if (const auto artist = artists.byId(artist_id)) {
      track.artists.push_back(
          ArtistLink{entities_.ref_for(EntityType::Artist, artist_id),
                     artist->name});
    }
  }

  if (const auto album = album_of(row)) {
    track.album = AlbumLink{entities_.ref_for(EntityType::Album, album->id),
                            album->name};
    track.year = album->year;
    repo::AlbumRepo albums(db_path_);
    for (const int artist_id : albums.artistIdsByAlbumId(album->id)) {
      if (const auto artist = artists.byId(artist_id)) {
        track.album_artists.push_back(AlbumArtistLink{
            entities_.ref_for(EntityType::AlbumArtist, artist_id),
            artist->name});
      }
    }
    const std::string cover = cover_path_of(track.ref);
    if (!cover.empty()) {
      track.artwork = ArtworkRef{
          entities_.ref_for(EntityType::Album, album->id), "primary",
          std::nullopt, std::to_string(std::hash<std::string>{}(cover))};
    }
  }

  {
    SQLite::Database &database = db::conn(db_path_);
    SQLite::Statement query(
        database, "SELECT 1 FROM track_lyrics WHERE track_id = ? AND text <> ''");
    query.bind(1, row.id);
    track.lyrics_available = query.executeStep();
  }

  return track;
}

Album LocalLibrary::build_album(const repo::Album &row) {
  Album album;
  album.ref = entities_.ref_for(EntityType::Album, row.id);
  album.title = row.name;
  album.year = row.year;

  repo::ArtistRepo artists(db_path_);
  for (const int artist_id : row.artist_ids) {
    if (const auto artist = artists.byId(artist_id)) {
      album.artists.push_back(AlbumArtistLink{
          entities_.ref_for(EntityType::AlbumArtist, artist_id),
          artist->name});
    }
  }

  if (!row.cover_path.empty()) {
    album.artwork = ArtworkRef{
        album.ref, "primary", std::nullopt,
        std::to_string(std::hash<std::string>{}(row.cover_path))};
  }
  return album;
}

Artist LocalLibrary::build_artist(const repo::Artist &row) {
  Artist artist;
  artist.ref = entities_.ref_for(EntityType::Artist, row.id);
  artist.name = row.name;
  artist.sort_name = row.sort_name;
  const std::string cover = cover_path_of(artist.ref);
  if (!cover.empty()) {
    artist.artwork = ArtworkRef{
        artist.ref, "primary", std::nullopt,
        std::to_string(std::hash<std::string>{}(cover))};
  }
  return artist;
}

Result<Track> LocalLibrary::get_track(const TrackRef &ref,
                                      const CallContext &) {
  if (ref.entity_type != EntityType::Track ||
      ref.library_id != library_id_) {
    return Result<Track>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid track reference"));
  }
  const auto internal_id = entities_.internal_id_of(ref);
  if (!internal_id) {
    return Result<Track>::failure(
        Error::make(ErrorCode::NotFound, "track not found"));
  }
  repo::TrackRepo tracks(db_path_);
  const auto row = tracks.byId(*internal_id);
  if (!row) {
    return Result<Track>::failure(
        Error::make(ErrorCode::NotFound, "track not found"));
  }
  return Result<Track>::success(build_track(*row));
}

Result<Album> LocalLibrary::get_album(const AlbumRef &ref,
                                      const CallContext &) {
  if (ref.entity_type != EntityType::Album ||
      ref.library_id != library_id_) {
    return Result<Album>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid album reference"));
  }
  const auto internal_id = entities_.internal_id_of(ref);
  if (!internal_id) {
    return Result<Album>::failure(
        Error::make(ErrorCode::NotFound, "album not found"));
  }
  repo::AlbumRepo albums(db_path_);
  const auto row = albums.byId(*internal_id);
  if (!row) {
    return Result<Album>::failure(
        Error::make(ErrorCode::NotFound, "album not found"));
  }
  return Result<Album>::success(build_album(*row));
}

Result<Artist> LocalLibrary::get_artist(const ArtistRef &ref,
                                        const CallContext &) {
  if (ref.entity_type != EntityType::Artist ||
      ref.library_id != library_id_) {
    return Result<Artist>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid artist reference"));
  }
  const auto internal_id = entities_.internal_id_of(ref);
  if (!internal_id) {
    return Result<Artist>::failure(
        Error::make(ErrorCode::NotFound, "artist not found"));
  }
  repo::ArtistRepo artists(db_path_);
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
  if (ref.entity_type != EntityType::Album ||
      ref.library_id != library_id_) {
    return Result<Page<Track>>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid album reference"));
  }
  const auto internal_id = entities_.internal_id_of(ref);
  if (!internal_id) {
    return Result<Page<Track>>::failure(
        Error::make(ErrorCode::NotFound, "album not found"));
  }
  repo::TrackRepo tracks(db_path_);
  std::vector<repo::Track> rows = tracks.byAlbum(*internal_id);
  Page<Track> out;
  const int limit = page.limit > 0 ? page.limit : 50;
  for (const auto &row : rows) {
    out.items.push_back(build_track(row));
    if (static_cast<int>(out.items.size()) >= limit) {
      break;
    }
  }
  out.has_more = static_cast<int>(rows.size()) > static_cast<int>(out.items.size());
  return Result<Page<Track>>::success(std::move(out));
}

Result<Page<Album>> LocalLibrary::list_artist_albums(const ArtistRef &ref,
                                                     const PageRequest &page,
                                                     const CallContext &) {
  if (ref.entity_type != EntityType::Artist ||
      ref.library_id != library_id_) {
    return Result<Page<Album>>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid artist reference"));
  }
  const auto internal_id = entities_.internal_id_of(ref);
  if (!internal_id) {
    return Result<Page<Album>>::failure(
        Error::make(ErrorCode::NotFound, "artist not found"));
  }
  repo::AlbumRepo albums(db_path_);
  std::vector<repo::Album> rows = albums.byArtist(*internal_id);
  Page<Album> out;
  const int limit = page.limit > 0 ? page.limit : 50;
  for (const auto &row : rows) {
    out.items.push_back(build_album(row));
    if (static_cast<int>(out.items.size()) >= limit) {
      break;
    }
  }
  out.has_more = static_cast<int>(rows.size()) > static_cast<int>(out.items.size());
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
  if (query.text.empty() ||
      (!want_tracks && !want_albums && !want_artists)) {
    return Result<Page<SearchHit>>::success(std::move(page));
  }

  const int limit = query.page.limit > 0 ? query.page.limit : 50;
  std::vector<SearchHit> hits;

  if (want_albums) {
    repo::AlbumRepo albums(db_path_);
    for (const auto &row : albums.search(query.text, limit)) {
      const Album album = build_album(row);
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
    repo::ArtistRepo artists(db_path_);
    for (const auto &row : artists.search(query.text, limit)) {
      const Artist artist = build_artist(row);
      SearchHit hit;
      hit.kind = EntityType::Artist;
      hit.ref = artist.ref;
      hit.title = artist.name;
      hit.artwork = artist.artwork;
      hits.push_back(std::move(hit));
    }
  }

  if (want_tracks) {
    repo::TrackRepo tracks(db_path_);
    for (const auto &row : tracks.search(query.text, limit)) {
      const Track track = build_track(row);
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

  page.items = std::move(hits);
  page.has_more = false;
  return Result<Page<SearchHit>>::success(std::move(page));
}

Result<PlaybackSource> LocalLibrary::resolve_playback(const TrackRef &ref,
                                                      const CallContext &) {
  if (ref.entity_type != EntityType::Track ||
      ref.library_id != library_id_) {
    return Result<PlaybackSource>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid track reference"));
  }
  const auto internal_id = entities_.internal_id_of(ref);
  if (!internal_id) {
    return Result<PlaybackSource>::failure(
        Error::make(ErrorCode::NotFound, "track not found"));
  }
  repo::TrackRepo tracks(db_path_);
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
  if (ref.entity_type != EntityType::Track ||
      ref.library_id != library_id_) {
    return Result<Lyrics>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid track reference"));
  }
  const auto internal_id = entities_.internal_id_of(ref);
  if (!internal_id) {
    return Result<Lyrics>::failure(
        Error::make(ErrorCode::NotFound, "track not found"));
  }

  std::string text;
  bool synced = false;
  {
    SQLite::Database &database = db::conn(db_path_);
    SQLite::Statement query(database, schema::sql::kTrackLyricsByTrackId);
    query.bind(1, *internal_id);
    if (!query.executeStep()) {
      return Result<Lyrics>::failure(
          Error::make(ErrorCode::NotFound, "lyrics not found"));
    }
    text = query.getColumn(0).getText();
    synced = query.getColumn(1).getInt() != 0;
  }
  if (text.empty()) {
    return Result<Lyrics>::failure(
        Error::make(ErrorCode::NotFound, "lyrics not found"));
  }

  Lyrics lyrics;
  lyrics.track = ref;
  lyrics.plain_text = text;

  std::istringstream stream(text);
  std::string raw_line;
  bool parsed_timestamp = false;
  while (std::getline(stream, raw_line)) {
    if (!raw_line.empty() && raw_line.back() == '\r') {
      raw_line.pop_back();
    }
    LyricLine line;
    if (synced && raw_line.size() > 2 && raw_line.front() == '[') {
      const std::size_t close = raw_line.find(']');
      if (close != std::string::npos) {
        const auto stamp =
            parse_timestamp(raw_line.substr(1, close - 1));
        if (stamp) {
          line.start_ms = *stamp;
          line.text = raw_line.substr(close + 1);
          parsed_timestamp = true;
          lyrics.lines.push_back(std::move(line));
          continue;
        }
      }
    }
    line.text = raw_line;
    lyrics.lines.push_back(std::move(line));
  }

  if (parsed_timestamp) {
    lyrics.mode = LyricsMode::LineSynced;
  } else {
    lyrics.mode = LyricsMode::Unsynced;
    for (auto &line : lyrics.lines) {
      line.start_ms.reset();
      line.end_ms.reset();
    }
  }
  return Result<Lyrics>::success(std::move(lyrics));
}

std::string LocalLibrary::cover_path_of(const SourceRef &owner) const {
  SQLite::Database &database = db::conn(db_path_);
  const auto internal_id = entities_.internal_id_of(owner);
  if (!internal_id) {
    return {};
  }

  if (owner.entity_type == EntityType::Album) {
    SQLite::Statement query(database,
                            "SELECT cover_path FROM album WHERE id = ?");
    query.bind(1, *internal_id);
    if (query.executeStep() && !query.getColumn(0).isNull()) {
      return query.getColumn(0).getText();
    }
    return {};
  }

  if (owner.entity_type == EntityType::Track) {
    SQLite::Statement query(
        database,
        "SELECT al.cover_path FROM track t JOIN album al ON al.id = t.album_id "
        "WHERE t.id = ?");
    query.bind(1, *internal_id);
    if (query.executeStep() && !query.getColumn(0).isNull()) {
      return query.getColumn(0).getText();
    }
    return {};
  }

  if (owner.entity_type == EntityType::Artist ||
      owner.entity_type == EntityType::AlbumArtist) {
    SQLite::Statement query(
        database,
        "SELECT al.cover_path FROM album al JOIN album_artist aa ON "
        "aa.album_id = al.id WHERE aa.artist_id = ? AND al.cover_path IS NOT "
        "NULL AND al.cover_path <> '' LIMIT 1");
    query.bind(1, *internal_id);
    if (query.executeStep()) {
      return query.getColumn(0).getText();
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

Result<ArtworkLocation>
LocalLibrary::artwork_location(const ArtworkRef &ref, const CallContext &) {
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
  SQLite::Database &database = db::conn(db_path_);

  std::unordered_map<int, std::string> artist_names;
  {
    SQLite::Statement query(database, "SELECT id, name FROM artist");
    while (query.executeStep()) {
      artist_names.emplace(query.getColumn(0).getInt(), query.getColumn(1).getText());
    }
  }

  std::unordered_map<int, std::vector<int>> album_artist_ids;
  {
    SQLite::Statement query(
        database, "SELECT album_id, artist_id FROM album_artist ORDER BY "
                  "album_id, position");
    while (query.executeStep()) {
      album_artist_ids[query.getColumn(0).getInt()].push_back(
          query.getColumn(1).getInt());
    }
  }

  std::unordered_map<int, std::vector<int>> track_artist_ids;
  {
    SQLite::Statement query(
        database, "SELECT track_id, artist_id FROM track_artist ORDER BY "
                  "track_id, position");
    while (query.executeStep()) {
      track_artist_ids[query.getColumn(0).getInt()].push_back(
          query.getColumn(1).getInt());
    }
  }

  std::unordered_map<int, std::vector<GenreEntry>> genre_by_track;
  std::unordered_set<std::string> seen_genres;
  {
    SQLite::Statement query(
        database,
        "SELECT tg.track_id, g.id, g.name FROM track_genre tg "
        "JOIN genre g ON g.id = tg.genre_id ORDER BY g.name");
    while (query.executeStep()) {
      const GenreEntry entry{query.getColumn(1).getInt(),
                             query.getColumn(2).getText()};
      genre_by_track[query.getColumn(0).getInt()].push_back(entry);
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
    SQLite::Statement query(
        database,
        "SELECT album_id, COUNT(*), COALESCE(SUM(duration_ms), 0) FROM track "
        "WHERE album_id > 0 GROUP BY album_id");
    while (query.executeStep()) {
      const int album_id = query.getColumn(0).getInt();
      album_count[album_id] = query.getColumn(1).getInt();
      album_duration[album_id] = query.getColumn(2).getInt64();
    }
  }

  std::unordered_set<int> lyrics_tracks;
  {
    SQLite::Statement query(
        database, "SELECT track_id FROM track_lyrics WHERE text <> ''");
    while (query.executeStep()) {
      lyrics_tracks.insert(query.getColumn(0).getInt());
    }
  }

  {
    SQLite::Statement query(
        database,
        "SELECT a.id, a.name, a.year, a.cover_path FROM album a ORDER BY a.name");
    while (query.executeStep()) {
      repo::Album row;
      row.id = query.getColumn(0).getInt();
      row.name = query.getColumn(1).getText();
      row.year = query.getColumn(2).getInt();
      row.cover_path =
          query.getColumn(3).isNull() ? std::string() : query.getColumn(3).getText();
      const auto it = album_artist_ids.find(row.id);
      if (it != album_artist_ids.end()) {
        row.artist_ids = it->second;
      }

      Album album;
      album.ref = entities_.ref_for(EntityType::Album, row.id);
      album.title = row.name;
      album.year = row.year > 0 ? std::optional<int>(row.year) : std::nullopt;
      for (const int artist_id : row.artist_ids) {
        const auto name = artist_names.find(artist_id);
        if (name == artist_names.end()) {
          continue;
        }
        album.artists.push_back(AlbumArtistLink{
            entities_.ref_for(EntityType::AlbumArtist, artist_id), name->second});
      }
      if (!row.cover_path.empty()) {
        album.artwork = ArtworkRef{
            entities_.ref_for(EntityType::Album, row.id), "primary", std::nullopt,
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
    SQLite::Statement query(
        database,
        "SELECT id, name, sort_name FROM artist ORDER BY "
        "COALESCE(NULLIF(sort_name, ''), name)");
    while (query.executeStep()) {
      Artist artist;
      artist.ref =
          entities_.ref_for(EntityType::Artist, query.getColumn(0).getInt());
      artist.name = query.getColumn(1).getText();
      artist.sort_name =
          query.getColumn(2).isNull() ? std::string() : query.getColumn(2).getText();
      out.artist_internal_ids.push_back(query.getColumn(0).getInt());
      out.artists.push_back(std::move(artist));
    }
  }

  for (const auto &kv : album_artist_ids) {
    for (const int artist_id : kv.second) {
      out.artist_album_count[artist_id] += 1;
    }
  }

  {
    SQLite::Statement query(
        database,
        "SELECT id, root_id, path, title, album_id, disc_no, track_no, "
        "duration_ms, bitrate, sample_rate, channels, format, size_bytes, mtime, "
        "added_at, play_count, last_played, rating FROM track "
        "ORDER BY album_id, disc_no, track_no, id");
    while (query.executeStep()) {
      repo::Track row;
      row.id = query.getColumn(0).getInt();
      row.root_id = query.getColumn(1).getInt();
      row.path = query.getColumn(2).isNull() ? std::string() : query.getColumn(2).getText();
      row.title = query.getColumn(3).isNull() ? std::string() : query.getColumn(3).getText();
      row.album_id = query.getColumn(4).getInt();
      row.disc_no = query.getColumn(5).getInt();
      row.track_no = query.getColumn(6).getInt();
      row.duration_ms = query.getColumn(7).getInt();
      row.bitrate = query.getColumn(8).getInt();
      row.sample_rate = query.getColumn(9).getInt();
      row.channels = query.getColumn(10).getInt();
      row.format = query.getColumn(11).isNull() ? std::string() : query.getColumn(11).getText();
      row.size_bytes = query.getColumn(12).getInt64();
      row.mtime = query.getColumn(13).getInt64();
      row.added_at = query.getColumn(14).isNull() ? std::string() : query.getColumn(14).getText();
      row.play_count = query.getColumn(15).getInt();
      row.last_played = query.getColumn(16).isNull() ? std::string() : query.getColumn(16).getText();
      row.rating = query.getColumn(17).getInt();

      const auto tracks_it = track_artist_ids.find(row.id);
      if (tracks_it != track_artist_ids.end()) {
        row.artist_ids = tracks_it->second;
      }

      Track track;
      track.ref = entities_.ref_for(EntityType::Track, row.id);
      track.title = row.title;
      track.duration_ms = row.duration_ms > 0
                              ? std::optional<long long>(row.duration_ms)
                              : std::nullopt;
      track.track_no = row.track_no > 0 ? std::optional<int>(row.track_no)
                                        : std::nullopt;
      track.disc_no =
          row.disc_no > 0 ? std::optional<int>(row.disc_no) : std::nullopt;
      track.user_data.play_count = row.play_count;
      track.user_data.last_played = row.last_played;
      track.user_data.rating = row.rating;
      track.user_data.added_at = row.added_at;
      track.audio.bitrate_kbps =
          row.bitrate > 0 ? std::optional<int>(row.bitrate) : std::nullopt;
      track.audio.sample_rate =
          row.sample_rate > 0 ? std::optional<int>(row.sample_rate) : std::nullopt;
      track.audio.channels =
          row.channels > 0 ? std::optional<int>(row.channels) : std::nullopt;
      track.audio.format = row.format;
      track.audio.size_bytes =
          row.size_bytes > 0 ? std::optional<long long>(row.size_bytes)
                             : std::nullopt;

      for (const int artist_id : row.artist_ids) {
        const auto name = artist_names.find(artist_id);
        if (name == artist_names.end()) {
          continue;
        }
        track.artists.push_back(ArtistLink{
            entities_.ref_for(EntityType::Artist, artist_id), name->second});
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
                entities_.ref_for(EntityType::AlbumArtist, artist_id),
                name->second});
          }
        }
        if (const auto cover = album_cover_tag.find(row.album_id);
            cover != album_cover_tag.end()) {
          track.artwork = ArtworkRef{entities_.ref_for(EntityType::Album,
                                                       row.album_id),
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
    SQLite::Statement query(
        database,
        "SELECT t.id FROM track t JOIN play_event pe ON t.id = pe.track_id "
        "ORDER BY pe.played_at DESC");
    while (query.executeStep()) {
      out.recent_play_track_ids.push_back(query.getColumn(0).getInt());
    }
  }

  return out;
}

std::optional<TrackTransport>
LocalLibrary::track_transport(int internal_id) const {
  SQLite::Database &database = db::conn(db_path_);
  TrackTransport transport;
  bool found = false;
  {
    SQLite::Statement query(
        database,
        "SELECT path, mtime, album_id FROM track WHERE id = ?");
    query.bind(1, internal_id);
    if (query.executeStep()) {
      transport.path =
          query.getColumn(0).isNull() ? std::string() : query.getColumn(0).getText();
      transport.mtime = query.getColumn(1).getInt64();
      transport.album_internal_id = query.getColumn(2).getInt();
      found = true;
    }
  }
  if (!found) {
    return std::nullopt;
  }
  {
    SQLite::Statement query(
        database,
        "SELECT artist_id FROM track_artist WHERE track_id = ? ORDER BY position");
    query.bind(1, internal_id);
    while (query.executeStep()) {
      transport.artist_internal_ids.push_back(query.getColumn(0).getInt());
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

} // namespace media_library
