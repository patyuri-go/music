#include "media_library/netease_library.h"

#include "api/api-enhanced/ids.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace media_library {
namespace {

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

// 封面身份用图片 URL 的哈希：同一张图得到同一个 tag，不同图必然不同。
// 仓库里没有 MD5 实现，这里沿用本地库的 std::hash 口径（见计划 §4.2）。
//
// 注意只用 URL 的 path：网易云图片 CDN 主机名会在 p1..p4 / m7 等之间轮换，
// 同一张图两次请求可能拿到不同 host；按整串哈希会让 ETag 跨请求漂移，
// 条件请求与客户端缓存全部失效。
std::string artwork_key(const std::string &url) {
  const std::size_t scheme = url.find("://");
  if (scheme == std::string::npos) {
    return url;
  }
  const std::size_t slash = url.find('/', scheme + 3);
  if (slash == std::string::npos) {
    return url;
  }
  return url.substr(slash);
}

std::string cache_tag_of(const std::string &key) {
  return std::to_string(std::hash<std::string>{}(artwork_key(key)));
}

// publishTime 是毫秒时间戳；0/缺省视为「无日期」，不猜。
std::optional<std::string> date_of(std::int64_t publish_time_ms) {
  if (publish_time_ms <= 0) {
    return std::nullopt;
  }
  const std::time_t seconds =
      static_cast<std::time_t>(publish_time_ms / 1000);
  std::tm utc{};
  if (gmtime_r(&seconds, &utc) == nullptr) {
    return std::nullopt;
  }
  char buffer[16] = {};
  const std::size_t written =
      std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &utc);
  if (written == 0) {
    return std::nullopt;
  }
  return std::string(buffer, written);
}

std::optional<int> year_of(std::int64_t publish_time_ms) {
  const auto date = date_of(publish_time_ms);
  if (!date.has_value() || date->size() < 4) {
    return std::nullopt;
  }
  return std::stoi(date->substr(0, 4));
}

// 分页游标：网易云全部是 offset 分页，游标就是十进制偏移量。
std::int64_t offset_of(const std::optional<std::string> &cursor) {
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

std::optional<int> positive_or_nullopt(int value) {
  return value > 0 ? std::optional<int>(value) : std::nullopt;
}

// tags 可能是空串、逗号分隔串或数组；只有非空项才进 genres。
std::vector<std::string> genres_of(const std::vector<std::string> &tags) {
  std::vector<std::string> genres;
  constexpr std::size_t kMaxGenres = 8;
  for (const std::string &tag : tags) {
    if (tag.empty()) {
      continue;
    }
    if (std::find(genres.begin(), genres.end(), tag) != genres.end()) {
      continue;
    }
    genres.push_back(tag);
    if (genres.size() >= kMaxGenres) {
      break;
    }
  }
  return genres;
}

// 音质档位从高到低：hr → sq → h → m → l。平台只给「可选清单」，
// 不代表当前账号真能播这一档，真实音质由 /song/url/v1 决定。
const api_enhanced::NeteaseQuality *
pick_quality(const api_enhanced::NeteaseSong &song) {
  if (song.hr.present) {
    return &song.hr;
  }
  if (song.sq.present) {
    return &song.sq;
  }
  if (song.h.present) {
    return &song.h;
  }
  if (song.m.present) {
    return &song.m;
  }
  if (song.l.present) {
    return &song.l;
  }
  return nullptr;
}

AudioInfo audio_of(const api_enhanced::NeteaseSong &song) {
  AudioInfo audio;
  const api_enhanced::NeteaseQuality *quality = pick_quality(song);
  if (quality == nullptr) {
    return audio;
  }
  if (quality->br > 0) {
    audio.bitrate_kbps = static_cast<int>(quality->br / 1000);
  }
  if (quality->sample_rate > 0) {
    audio.sample_rate = static_cast<int>(quality->sample_rate);
  }
  if (quality->size > 0) {
    audio.size_bytes = quality->size;
  }
  // hr/sq 是无损档（fid 决定容器），其余是 mp3。
  const bool lossless = quality == &song.hr || quality == &song.sq;
  audio.format = lossless ? "flac" : "mp3";
  return audio;
}

std::string mime_of(const std::string &format) {
  if (format == "flac" || format == "wav" || format == "ape") {
    return "audio/flac";
  }
  return "audio/mpeg";
}

// 名字过滤沿用本地库语义：小写化后全等，避免「周杰伦」命中「周杰伦的徒弟」。
bool contains_name(const std::vector<ArtistLink> &links,
                   const std::string &needle) {
  return std::any_of(links.begin(), links.end(),
                     [&needle](const ArtistLink &link) {
                       return link.name.size() == needle.size() &&
                              std::equal(link.name.begin(), link.name.end(),
                                         needle.begin(), [](char a, char b) {
                                           return std::tolower(
                                                      static_cast<unsigned char>(a)) ==
                                                  std::tolower(
                                                      static_cast<unsigned char>(b));
                                         });
                     });
}

bool contains_name(const std::vector<AlbumArtistLink> &links,
                   const std::string &needle) {
  return std::any_of(links.begin(), links.end(),
                     [&needle](const AlbumArtistLink &link) {
                       return link.name.size() == needle.size() &&
                              std::equal(link.name.begin(), link.name.end(),
                                         needle.begin(), [](char a, char b) {
                                           return std::tolower(
                                                      static_cast<unsigned char>(a)) ==
                                                  std::tolower(
                                                      static_cast<unsigned char>(b));
                                         });
                     });
}

} // namespace

NeteaseLibrary::NeteaseLibrary(
    std::string library_id, std::string display_name,
    std::shared_ptr<api_enhanced::NeteaseClient> client, std::string uid)
    : library_id_(std::move(library_id)),
      display_name_(std::move(display_name)), client_(std::move(client)),
      uid_(std::move(uid)) {}

void NeteaseLibrary::initialize() {
  auto self = shared_from_this();
  // 注册顺序与 LocalLibrary 一致：目录 → 搜索 → 播放 → 歌词 → 封面。
  register_capability<CatalogReader>(self);
  register_capability<Searcher>(self);
  register_capability<StreamResolver>(self);
  register_capability<LyricsProvider>(self);
  register_capability<ArtworkProvider>(self);
  register_capability<ArtworkBytes>(self);
  register_capability<PlaylistReader>(self);
}

LibraryIdentity NeteaseLibrary::identity() const {
  return LibraryIdentity{library_id_, display_name_, "netease"};
}

CapabilitySet NeteaseLibrary::capabilities() const {
  CapabilitySet set;
  // 11 项全部显式列出：缺项会被上层当成「没声明」而静默跳过，
  // 显式的 false 才能让 /api/v1/libraries 如实反映现状。
  const auto add = [&set](CapabilityId id, bool supported,
                          std::optional<ErrorCode> reason) {
    CapabilityState state;
    state.id = id;
    state.supported = supported;
    state.available = supported;
    state.reason = reason;
    set.items.push_back(std::move(state));
  };
  // N2 打开只读目录、搜索与直链解析；N4 补歌词与封面；歌单在 N5。
  add(CapabilityId::CatalogRead, true, std::nullopt);
  add(CapabilityId::Search, true, std::nullopt);
  add(CapabilityId::StreamResolve, true, std::nullopt);
  add(CapabilityId::LyricsRead, true, std::nullopt);
  add(CapabilityId::ArtworkRead, true, std::nullopt);
  add(CapabilityId::PlaylistRead, true, std::nullopt);
  add(CapabilityId::PlaylistWrite, false, ErrorCode::Unsupported);
  add(CapabilityId::FavoriteRead, false, ErrorCode::Unsupported);
  add(CapabilityId::FavoriteWrite, false, ErrorCode::Unsupported);
  add(CapabilityId::LibraryWrite, false, ErrorCode::Unsupported);
  add(CapabilityId::Auth, false, ErrorCode::Unsupported);
  return set;
}

void NeteaseLibrary::close() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  client_.reset();
  login_.reset();
}

HealthSnapshot NeteaseLibrary::health() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return health_;
}

std::optional<api_enhanced::NeteaseLoginStatus>
NeteaseLibrary::login_snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return login_;
}

bool NeteaseLibrary::member_active() const {
  const auto login = login_snapshot();
  // vipType：0 普通用户；>0 是音乐包/畅听包等付费身份。
  return login.has_value() && login->authenticated && login->vip_type > 0;
}

void NeteaseLibrary::probe_health(const CallContext &ctx) {
  HealthSnapshot health;
  std::shared_ptr<api_enhanced::NeteaseClient> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = client_;
  }

  std::optional<api_enhanced::NeteaseLoginStatus> login;
  // 探测失败不是构造失败：把库标成不可用后照常注册，列表里仍能看到它。
  if (client == nullptr) {
    health.state = HealthState::Unavailable;
    health.reason = ErrorCode::ProviderUnavailable;
  } else {
    auto status = client->login_status(ctx);
    if (!status.ok()) {
      health.state = HealthState::Unavailable;
      health.reason = status.error().code;
    } else {
      login = status.value();
      if (status.value().authenticated) {
        health.state = HealthState::Ok;
      } else {
        // 匿名也能搜索、拿公开歌单、播免费曲，所以不是故障。
        health.state = HealthState::Unauthenticated;
        health.reason = ErrorCode::AuthRequired;
      }
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  health_ = health;
  login_ = std::move(login);
}

Track NeteaseLibrary::track_from_song(
    const api_enhanced::NeteaseSong &song,
    const std::optional<api_enhanced::NeteaseAlbumBrief> &album,
    const CallContext &) const {
  Track track;
  const auto ref = api_enhanced::make_ref(EntityType::Track, song.id);
  if (!ref.ok()) {
    return track;
  }
  track.ref = ref.value();
  track.title = song.name;
  track.duration_ms =
      song.duration_ms > 0 ? std::optional<std::int64_t>(song.duration_ms)
                           : std::nullopt;
  track.track_no = positive_or_nullopt(song.no);
  track.disc_no = positive_or_nullopt(song.cd);
  track.date = date_of(song.publish_time_ms);
  track.year = year_of(song.publish_time_ms);

  for (const auto &artist : song.artists) {
    auto artist_ref = api_enhanced::make_ref(EntityType::Artist, artist.id);
    if (!artist_ref.ok()) {
      continue;
    }
    track.artists.push_back(
        ArtistLink{artist_ref.value(), artist.name});
  }

  // al{} 只有 id/name/picUrl；专辑艺术家必须来自 /album 的 artists[]，
  // 拿不到时降级成曲目艺术家（不失败，只记一条日志）。
  api_enhanced::NeteaseAlbumBrief brief;
  if (album.has_value()) {
    brief = *album;
  } else {
    brief = song.album;
  }

  if (song.album.id > 0) {
    auto album_ref = api_enhanced::make_ref(EntityType::Album, song.album.id);
    if (album_ref.ok()) {
      track.album = AlbumLink{album_ref.value(), song.album.name};
    }
  }

  const std::vector<api_enhanced::NeteaseArtistRef> *album_artists =
      &brief.artists;
  if (album_artists->empty()) {
    // 拿不到专辑艺术家：降级为曲目艺术家，保持 album_artists 非空。
    album_artists = &song.artists;
    if (!song.artists.empty()) {
      std::cerr << "[netease] album artists fallback to track artists (track "
                << song.id << ")\n";
    }
  }
  for (const auto &artist : *album_artists) {
    auto artist_ref =
        api_enhanced::make_ref(EntityType::AlbumArtist, artist.id);
    if (!artist_ref.ok()) {
      continue;
    }
    track.album_artists.push_back(
        AlbumArtistLink{artist_ref.value(), artist.name});
  }

  // 封面 owner 是专辑：网易云封面本来就是专辑封面。
  const std::string &picture =
      !brief.pic_url.empty() ? brief.pic_url : song.album.pic_url;
  if (song.album.id > 0 && !picture.empty()) {
    auto owner = api_enhanced::make_ref(EntityType::Album, song.album.id);
    if (owner.ok()) {
      track.artwork =
          ArtworkRef{owner.value(), "primary", std::nullopt,
                     cache_tag_of(picture)};
    }
  }

  track.audio = audio_of(song);
  // 歌词按需拉取，这里只声明「有」。
  track.lyrics_available = true;

  // §4.6：限制是限制，故障是故障。只有 fee==4（需购买专辑）与
  // fee==1 且非会员才是「不可播」。
  if (song.fee == 4) {
    track.playable = false;
    track.restrictions.push_back(
        Restriction{RestrictionCode::CopyrightUnavailable, "需购买专辑"});
  } else if (song.fee == 1 && !member_active()) {
    track.playable = false;
    track.restrictions.push_back(
        Restriction{RestrictionCode::VipRequired, "需要会员"});
  }
  return track;
}

Album
NeteaseLibrary::album_from_brief(const api_enhanced::NeteaseAlbumBrief &brief) const {
  Album album;
  const auto ref = api_enhanced::make_ref(EntityType::Album, brief.id);
  if (!ref.ok()) {
    return album;
  }
  album.ref = ref.value();
  album.title = brief.name;
  album.date = date_of(brief.publish_time_ms);
  album.year = year_of(brief.publish_time_ms);
  album.genres = genres_of(brief.tags);
  for (const auto &artist : brief.artists) {
    auto artist_ref =
        api_enhanced::make_ref(EntityType::AlbumArtist, artist.id);
    if (!artist_ref.ok()) {
      continue;
    }
    album.artists.push_back(
        AlbumArtistLink{artist_ref.value(), artist.name});
  }
  if (brief.id > 0 && !brief.pic_url.empty()) {
    album.artwork =
        ArtworkRef{album.ref, "primary", std::nullopt,
                   cache_tag_of(brief.pic_url)};
  }
  return album;
}

Artist
NeteaseLibrary::artist_from_brief(const api_enhanced::NeteaseArtist &brief) const {
  Artist artist;
  const auto ref = api_enhanced::make_ref(EntityType::Artist, brief.id);
  if (!ref.ok()) {
    return artist;
  }
  artist.ref = ref.value();
  artist.name = brief.name;
  artist.sort_name = brief.name;
  if (!brief.pic_url.empty()) {
    artist.artwork =
        ArtworkRef{artist.ref, "primary", std::nullopt,
                   cache_tag_of(brief.pic_url)};
  }
  return artist;
}

Result<Track> NeteaseLibrary::get_track(const TrackRef &ref,
                                        const CallContext &ctx) {
  using R = Result<Track>;
  if (ref.entity_type != EntityType::Track || ref.library_id != library_id_) {
    return R::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid track reference"));
  }
  const auto id = api_enhanced::parse_platform_id(ref.entity_id);
  if (!id.ok()) {
    return R::failure(id.error());
  }

  std::shared_ptr<api_enhanced::NeteaseClient> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = client_;
  }
  if (client == nullptr) {
    return R::failure(
        Error::make(ErrorCode::ProviderUnavailable, "netease client is closed"));
  }

  auto songs = client->song_detail({id.value()}, ctx);
  if (!songs.ok()) {
    return R::failure(songs.error());
  }
  const auto &list = songs.value();
  const auto it = std::find_if(list.begin(), list.end(),
                               [&id](const api_enhanced::NeteaseSong &song) {
                                 return song.id == id.value();
                               });
  if (it == list.end()) {
    return R::failure(
        Error::make(ErrorCode::NotFound, "netease track not found"));
  }

  // 专辑艺术家必须额外反查一次 /album（命中进程内缓存时零开销）。
  // 失败只降级，不让整次 get_track 失败。
  std::optional<api_enhanced::NeteaseAlbumBrief> brief;
  if (it->album.id > 0) {
    auto album = client->album(it->album.id, ctx);
    if (album.ok()) {
      brief = album.value().brief;
    } else {
      std::cerr << "[netease] album lookup failed for track " << it->id << ": "
                << error_code_name(album.error().code) << "\n";
    }
  }

  return R::success(track_from_song(*it, brief, ctx));
}

Result<Album> NeteaseLibrary::get_album(const AlbumRef &ref,
                                        const CallContext &ctx) {
  using R = Result<Album>;
  if (ref.entity_type != EntityType::Album || ref.library_id != library_id_) {
    return R::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid album reference"));
  }
  const auto id = api_enhanced::parse_platform_id(ref.entity_id);
  if (!id.ok()) {
    return R::failure(id.error());
  }

  std::shared_ptr<api_enhanced::NeteaseClient> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = client_;
  }
  if (client == nullptr) {
    return R::failure(
        Error::make(ErrorCode::ProviderUnavailable, "netease client is closed"));
  }

  auto album = client->album(id.value(), ctx);
  if (!album.ok()) {
    return R::failure(album.error());
  }
  if (album.value().brief.id <= 0) {
    return R::failure(
        Error::make(ErrorCode::NotFound, "netease album not found"));
  }
  return R::success(album_from_brief(album.value().brief));
}

Result<Artist> NeteaseLibrary::get_artist(const ArtistRef &ref,
                                          const CallContext &ctx) {
  using R = Result<Artist>;
  if (ref.entity_type != EntityType::Artist || ref.library_id != library_id_) {
    return R::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid artist reference"));
  }
  const auto id = api_enhanced::parse_platform_id(ref.entity_id);
  if (!id.ok()) {
    return R::failure(id.error());
  }

  std::shared_ptr<api_enhanced::NeteaseClient> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = client_;
  }
  if (client == nullptr) {
    return R::failure(
        Error::make(ErrorCode::ProviderUnavailable, "netease client is closed"));
  }

  auto artist = client->artist(id.value(), ctx);
  if (!artist.ok()) {
    return R::failure(artist.error());
  }
  if (artist.value().id <= 0) {
    return R::failure(
        Error::make(ErrorCode::NotFound, "netease artist not found"));
  }
  return R::success(artist_from_brief(artist.value()));
}

Result<Page<Track>> NeteaseLibrary::list_album_tracks(const AlbumRef &ref,
                                                      const PageRequest &page,
                                                      const CallContext &ctx) {
  using R = Result<Page<Track>>;
  if (ref.entity_type != EntityType::Album || ref.library_id != library_id_) {
    return R::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid album reference"));
  }
  const auto id = api_enhanced::parse_platform_id(ref.entity_id);
  if (!id.ok()) {
    return R::failure(id.error());
  }

  std::shared_ptr<api_enhanced::NeteaseClient> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = client_;
  }
  if (client == nullptr) {
    return R::failure(
        Error::make(ErrorCode::ProviderUnavailable, "netease client is closed"));
  }

  auto album = client->album(id.value(), ctx);
  if (!album.ok()) {
    return R::failure(album.error());
  }
  if (album.value().brief.id <= 0) {
    return R::failure(
        Error::make(ErrorCode::NotFound, "netease album not found"));
  }

  // 上游一次给全，分页只是在本地切片；has_more / next_cursor 如实反映剩余。
  const auto &songs = album.value().songs;
  const std::int64_t offset = offset_of(page.cursor);
  const int limit = page.limit > 0 ? page.limit : 50;

  Page<Track> out;
  for (std::int64_t i = offset;
       i < static_cast<std::int64_t>(songs.size()) &&
       static_cast<int>(out.items.size()) < limit;
       ++i) {
    out.items.push_back(track_from_song(songs[static_cast<std::size_t>(i)],
                                        album.value().brief, ctx));
  }
  if (offset + static_cast<std::int64_t>(out.items.size()) <
      static_cast<std::int64_t>(songs.size())) {
    out.has_more = true;
    out.next_cursor = std::to_string(offset +
                                     static_cast<std::int64_t>(out.items.size()));
  }
  return R::success(std::move(out));
}

Result<Page<Album>> NeteaseLibrary::list_artist_albums(const ArtistRef &ref,
                                                       const PageRequest &page,
                                                       const CallContext &ctx) {
  using R = Result<Page<Album>>;
  if (ref.entity_type != EntityType::Artist || ref.library_id != library_id_) {
    return R::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid artist reference"));
  }
  const auto id = api_enhanced::parse_platform_id(ref.entity_id);
  if (!id.ok()) {
    return R::failure(id.error());
  }

  std::shared_ptr<api_enhanced::NeteaseClient> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = client_;
  }
  if (client == nullptr) {
    return R::failure(
        Error::make(ErrorCode::ProviderUnavailable, "netease client is closed"));
  }

  const std::int64_t offset = offset_of(page.cursor);
  const int limit = std::clamp(page.limit > 0 ? page.limit : 50, 1, 100);

  auto albums = client->artist_albums(
      id.value(), limit, static_cast<int>(offset), ctx);
  if (!albums.ok()) {
    return R::failure(albums.error());
  }

  Page<Album> out;
  out.items.reserve(albums.value().items.size());
  for (const auto &brief : albums.value().items) {
    if (brief.id <= 0) {
      continue;
    }
    out.items.push_back(album_from_brief(brief));
  }
  out.has_more = albums.value().has_more;
  if (out.has_more) {
    out.next_cursor =
        std::to_string(offset + static_cast<std::int64_t>(out.items.size()));
  }
  return R::success(std::move(out));
}

Result<Page<SearchHit>> NeteaseLibrary::search(const SearchQuery &query,
                                               const CallContext &ctx) {
  using R = Result<Page<SearchHit>>;
  if (query.text.empty()) {
    return R::success(Page<SearchHit>{});
  }

  std::shared_ptr<api_enhanced::NeteaseClient> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = client_;
  }
  if (client == nullptr) {
    return R::failure(
        Error::make(ErrorCode::ProviderUnavailable, "netease client is closed"));
  }

  const bool all_types = query.types.empty();
  const auto wants = [&query, all_types](EntityType type) {
    return all_types ||
           std::find(query.types.begin(), query.types.end(), type) !=
               query.types.end();
  };
  const bool want_tracks = wants(EntityType::Track);
  const bool want_albums = wants(EntityType::Album);
  const bool want_artists =
      wants(EntityType::Artist) || wants(EntityType::AlbumArtist);
  if (!want_tracks && !want_albums && !want_artists) {
    return R::success(Page<SearchHit>{});
  }

  const int limit = std::clamp(query.page.limit > 0 ? query.page.limit : 50, 1,
                               100);
  const int offset = static_cast<int>(offset_of(query.page.cursor));

  Page<SearchHit> out;
  bool has_more = false;
  std::optional<Error> first_error;

  const auto note_error = [&first_error](const Error &error) {
    if (!first_error.has_value()) {
      first_error = error;
    }
  };

  if (want_tracks) {
    auto page_result = client->search_songs(query.text, limit, offset, ctx);
    if (!page_result.ok()) {
      note_error(page_result.error());
    } else {
      for (const auto &song : page_result.value().items) {
        Track track = track_from_song(song, std::nullopt, ctx);
        if (track.ref.entity_id.empty()) {
          continue;
        }
        if (!query.artists.empty() &&
            !contains_name(track.artists, query.artists.front())) {
          continue;
        }
        if (!query.album_artists.empty() &&
            !contains_name(track.album_artists, query.album_artists.front())) {
          continue;
        }
        if (!track.playable && !query.include_unplayable) {
          continue;
        }
        SearchHit hit;
        hit.kind = EntityType::Track;
        hit.ref = track.ref;
        hit.title = track.title;
        if (!track.artists.empty()) {
          hit.subtitle = track.artists.front().name;
        }
        hit.artists = track.artists;
        hit.album_artists = track.album_artists;
        hit.artwork = track.artwork;
        hit.duration_ms = track.duration_ms;
        hit.playable = track.playable;
        out.items.push_back(std::move(hit));
      }
      has_more = has_more || page_result.value().has_more;
    }
  }

  if (want_albums) {
    auto page_result = client->search_albums(query.text, limit, offset, ctx);
    if (!page_result.ok()) {
      note_error(page_result.error());
    } else {
      for (const auto &brief : page_result.value().items) {
        Album album = album_from_brief(brief);
        if (album.ref.entity_id.empty()) {
          continue;
        }
        if (!query.artists.empty() &&
            !contains_name(album.artists, query.artists.front())) {
          continue;
        }
        if (!query.album_artists.empty() &&
            !contains_name(album.artists, query.album_artists.front())) {
          continue;
        }
        SearchHit hit;
        hit.kind = EntityType::Album;
        hit.ref = album.ref;
        hit.title = album.title;
        if (!album.artists.empty()) {
          hit.subtitle = album.artists.front().name;
        }
        hit.album_artists = album.artists;
        hit.artwork = album.artwork;
        out.items.push_back(std::move(hit));
      }
      has_more = has_more || page_result.value().has_more;
    }
  }

  if (want_artists) {
    auto page_result = client->search_artists(query.text, limit, offset, ctx);
    if (!page_result.ok()) {
      note_error(page_result.error());
    } else {
      // 只要 Artist 时给出 Artist 引用；只点名 AlbumArtist 时给出
      // AlbumArtist 引用（同一个平台 ID，两种角色由调用方声明）。
      const bool as_album_artist =
          std::find(query.types.begin(), query.types.end(),
                    EntityType::AlbumArtist) != query.types.end() &&
          std::find(query.types.begin(), query.types.end(),
                    EntityType::Artist) == query.types.end();
      for (const auto &brief : page_result.value().items) {
        Artist artist = artist_from_brief(brief);
        if (artist.ref.entity_id.empty()) {
          continue;
        }
        if (!query.artists.empty() &&
            !contains_name(std::vector<ArtistLink>{
                               ArtistLink{artist.ref, artist.name}},
                           query.artists.front())) {
          continue;
        }
        SearchHit hit;
        if (as_album_artist) {
          auto album_artist_ref = api_enhanced::make_ref(
              EntityType::AlbumArtist, brief.id);
          if (!album_artist_ref.ok()) {
            continue;
          }
          hit.kind = EntityType::AlbumArtist;
          hit.ref = album_artist_ref.value();
          hit.album_artists.push_back(
              AlbumArtistLink{album_artist_ref.value(), artist.name});
        } else {
          hit.kind = EntityType::Artist;
          hit.ref = artist.ref;
          hit.artists.push_back(ArtistLink{artist.ref, artist.name});
        }
        hit.title = artist.name;
        hit.artwork = artist.artwork;
        out.items.push_back(std::move(hit));
      }
      has_more = has_more || page_result.value().has_more;
    }
  }

  // 只有「一个来源都没成功」才是故障；部分失败照常返回已有结果。
  if (out.items.empty() && first_error.has_value()) {
    return R::failure(*first_error);
  }
  out.has_more = has_more;
  if (has_more) {
    out.next_cursor = std::to_string(offset + limit);
  }
  return R::success(std::move(out));
}

Result<PlaybackSource>
NeteaseLibrary::resolve_playback(const TrackRef &ref, const CallContext &ctx) {
  using R = Result<PlaybackSource>;
  if (ref.entity_type != EntityType::Track || ref.library_id != library_id_) {
    return R::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid track reference"));
  }
  const auto id = api_enhanced::parse_platform_id(ref.entity_id);
  if (!id.ok()) {
    return R::failure(id.error());
  }

  std::shared_ptr<api_enhanced::NeteaseClient> client;
  std::string level;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = client_;
    level = client_ != nullptr ? client_->options().default_level : "exhigh";
  }
  if (client == nullptr) {
    return R::failure(
        Error::make(ErrorCode::ProviderUnavailable, "netease client is closed"));
  }

  // fee==4 的曲目平台不会给直链，先按「限制」而不是「故障」返回。
  auto songs = client->song_detail({id.value()}, ctx);
  if (songs.ok()) {
    const auto &list = songs.value();
    const auto it = std::find_if(list.begin(), list.end(),
                                 [&id](const api_enhanced::NeteaseSong &song) {
                                   return song.id == id.value();
                                 });
    if (it != list.end() && it->fee == 4) {
      Error error = Error::make(ErrorCode::PermissionDenied,
                               "netease track requires album purchase");
      error.restriction = RestrictionCode::CopyrightUnavailable;
      return R::failure(std::move(error));
    }
  }

  auto url = client->song_url(id.value(), level, ctx);
  if (!url.ok()) {
    return R::failure(url.error());
  }
  if (url.value().url.empty()) {
    // 平台明确表示「这首播不了」：版权/会员/地区，属于限制不是故障。
    Error error = Error::make(ErrorCode::PermissionDenied,
                             "netease track is not playable");
    error.restriction = url.value().fee == 1
                            ? RestrictionCode::VipRequired
                            : RestrictionCode::CopyrightUnavailable;
    return R::failure(std::move(error));
  }

  // §4.6：VIP 曲 + 非会员 = 不可播。未登录时平台会给一段 45 秒试听直链
  // （freeTrialInfo 非空），但按 §4.6 的限制矩阵这仍是「拒绝」，不是
  // 「播一段」。会员用户拿到的试听片段（例如超出其音质权的等级）才按
  // 试听处理（见下方 §8.1 的 trial 分支）。
  if (url.value().free_trial && url.value().fee == 1 && !member_active()) {
    Error error = Error::make(ErrorCode::PermissionDenied,
                              "netease track requires a vip membership");
    error.restriction = RestrictionCode::VipRequired;
    return R::failure(std::move(error));
  }

  PlaybackSource source;
  source.track = ref;
  source.kind = PlaybackKind::RemoteStream;
  source.quality =
      url.value().level_name.empty()
          ? std::optional<std::string>(level)
          : std::optional<std::string>(url.value().level_name);
  if (url.value().bitrate > 0) {
    source.bitrate_kbps = static_cast<int>(url.value().bitrate / 1000);
  }
  source.mime_type = mime_of(url.value().type);
  if (url.value().size > 0) {
    source.content_length = url.value().size;
  }
  source.range_supported = true;
  source.access_mode = AccessMode::Proxy;
  // 直链是短时凭据，绝不缓存进任何持久层。
  source.cacheable = false;

  // 留 60 秒余量，且至少给 60 秒，避免 expi 异常小导致立刻过期。
  const std::int64_t ttl_ms =
      std::max<std::int64_t>(60000, url.value().expi_seconds * 1000 - 60000);
  source.expires_at = now_ms() + ttl_ms;

  if (url.value().free_trial) {
    source.restrictions.push_back(
        Restriction{RestrictionCode::VipRequired, "仅可试听"});
  }

  // locator 只进会话表，不出进程、不进 HTTP 响应。
  source.internal_locator = url.value().url;
  return R::success(std::move(source));
}

std::optional<std::string>
NeteaseLibrary::picture_of(const SourceRef &owner,
                           const CallContext &ctx) const {
  std::shared_ptr<api_enhanced::NeteaseClient> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = client_;
  }
  if (client == nullptr) {
    return std::nullopt;
  }
  const auto id = api_enhanced::platform_id_of(owner);
  if (!id.has_value()) {
    return std::nullopt;
  }

  switch (owner.entity_type) {
  case EntityType::Album: {
    auto album = client->album(id.value(), ctx);
    if (album.ok() && !album.value().brief.pic_url.empty()) {
      return album.value().brief.pic_url;
    }
    return std::nullopt;
  }
  case EntityType::Track: {
    auto songs = client->song_detail({id.value()}, ctx);
    if (!songs.ok()) {
      return std::nullopt;
    }
    for (const auto &song : songs.value()) {
      if (song.id == id.value() && !song.album.pic_url.empty()) {
        return song.album.pic_url;
      }
    }
    return std::nullopt;
  }
  case EntityType::Playlist: {
    auto playlist = client->playlist(id.value(), ctx);
    if (playlist.ok() && !playlist.value().cover_img_url.empty()) {
      return playlist.value().cover_img_url;
    }
    return std::nullopt;
  }
  case EntityType::Artist:
  case EntityType::AlbumArtist: {
    auto artist = client->artist(id.value(), ctx);
    if (artist.ok() && !artist.value().pic_url.empty()) {
      return artist.value().pic_url;
    }
    return std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

Result<ArtworkRef> NeteaseLibrary::get_artwork(const SourceRef &owner,
                                               const CallContext &ctx) {
  if (owner.library_id != library_id_) {
    return Result<ArtworkRef>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid artwork owner"));
  }

  // 曲目的封面就是专辑封面：把 owner 换成专辑，与 Track::artwork 的
  // owner/cache_tag 口径完全一致（同一个 ETag，不会因 CDN 域名不同而漂移）。
  SourceRef target = owner;
  if (owner.entity_type == EntityType::Track) {
    std::shared_ptr<api_enhanced::NeteaseClient> client;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      client = client_;
    }
    const auto id = api_enhanced::platform_id_of(owner);
    if (client != nullptr && id.has_value()) {
      auto songs = client->song_detail({id.value()}, ctx);
      if (songs.ok()) {
        for (const auto &song : songs.value()) {
          if (song.id != id.value() || song.album.id <= 0) {
            continue;
          }
          auto album_ref =
              api_enhanced::make_ref(EntityType::Album, song.album.id);
          if (album_ref.ok()) {
            target = std::move(album_ref).value();
          }
          break;
        }
      }
    }
  }

  const std::optional<std::string> picture = picture_of(target, ctx);
  if (!picture.has_value() || picture->empty()) {
    return Result<ArtworkRef>::failure(
        Error::make(ErrorCode::NotFound, "netease artwork not found"));
  }
  return Result<ArtworkRef>::success(
      ArtworkRef{target, "primary", std::nullopt, cache_tag_of(*picture)});
}

Result<ArtworkLocation>
NeteaseLibrary::artwork_location(const ArtworkRef &ref, const CallContext &ctx) {
  const std::optional<std::string> picture = picture_of(ref.owner, ctx);
  if (!picture.has_value() || picture->empty()) {
    return Result<ArtworkLocation>::failure(
        Error::make(ErrorCode::NotFound, "netease artwork not found"));
  }
  ArtworkLocation location;
  // locator 只进 ArtworkService；cache_virtual_path 留空（远端没有本地缓存键）。
  location.locator = *picture;
  location.mime_type = "image/jpeg";
  return Result<ArtworkLocation>::success(std::move(location));
}

Result<Lyrics> NeteaseLibrary::get_lyrics(const TrackRef &ref,
                                          const CallContext &ctx) {
  using R = Result<Lyrics>;
  if (ref.entity_type != EntityType::Track || ref.library_id != library_id_) {
    return R::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid track reference"));
  }
  const auto id = api_enhanced::parse_platform_id(ref.entity_id);
  if (!id.ok()) {
    return R::failure(id.error());
  }

  std::shared_ptr<api_enhanced::NeteaseClient> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = client_;
  }
  if (client == nullptr) {
    return R::failure(
        Error::make(ErrorCode::ProviderUnavailable, "netease client is closed"));
  }

  auto parsed = client->lyric(id.value(), ctx);
  if (!parsed.ok()) {
    return R::failure(parsed.error());
  }
  if (parsed.value().lines.empty()) {
    // 无歌词（含纯音乐）：与本地库一致，返回 NotFound 让门面给 404。
    return R::failure(Error::make(ErrorCode::NotFound, "lyrics not found"));
  }

  Lyrics lyrics;
  lyrics.track = ref;
  lyrics.mode = parsed.value().word_synced
                    ? LyricsMode::WordSynced
                    : (parsed.value().line_synced ? LyricsMode::LineSynced
                                                  : LyricsMode::Unsynced);
  if (!parsed.value().plain_text.empty()) {
    lyrics.plain_text = parsed.value().plain_text;
  }
  for (const auto &line : parsed.value().lines) {
    LyricLine out;
    out.start_ms = line.start_ms;
    out.end_ms = line.end_ms;
    out.text = line.text;
    for (const auto &word : line.words) {
      LyricWord converted;
      converted.start_ms = word.start_ms;
      converted.end_ms = word.end_ms;
      converted.text = word.text;
      out.words.push_back(std::move(converted));
    }
    lyrics.lines.push_back(std::move(out));
  }
  return R::success(std::move(lyrics));
}

Result<Page<Playlist>>
NeteaseLibrary::list_playlists(const PageRequest &page,
                               const CallContext &ctx) {
  using R = Result<Page<Playlist>>;
  if (uid_.empty()) {
    // §12 N5：没配 uid 不是「没数据」，是可操作的配置错误。
    return R::failure(Error::make(
        ErrorCode::InvalidRequest,
        "netease uid is not configured (set netease.uid or --netease-uid)"));
  }

  std::shared_ptr<api_enhanced::NeteaseClient> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = client_;
  }
  if (client == nullptr) {
    return R::failure(
        Error::make(ErrorCode::ProviderUnavailable, "netease client is closed"));
  }

  const std::int64_t offset = offset_of(page.cursor);
  const int limit = page.limit > 0 ? page.limit : 50;

  auto fetched = client->user_playlists(uid_, limit,
                                        static_cast<int>(offset), ctx);
  if (!fetched.ok()) {
    return R::failure(fetched.error());
  }

  Page<Playlist> out;
  for (const auto &item : fetched.value().items) {
    if (item.id <= 0) {
      continue;
    }
    auto ref = api_enhanced::make_ref(EntityType::Playlist, item.id);
    if (!ref.ok()) {
      continue;
    }
    Playlist playlist;
    playlist.ref = std::move(ref).value();
    playlist.name = item.name;
    playlist.description = item.description;
    // 列表只给摘要，tracks 留给 get_playlist 按需展开。
    out.items.push_back(std::move(playlist));
  }

  // 平台的 more 是「还有下一批」的唯一依据；limit 不严格时额外按是否
  // 取满一页兜底，避免 has_more 永远为 true。
  if (!out.items.empty() &&
      (fetched.value().has_more ||
       static_cast<int>(out.items.size()) >= limit)) {
    out.has_more = true;
    out.next_cursor = std::to_string(
        offset + static_cast<std::int64_t>(out.items.size()));
  }
  return R::success(std::move(out));
}

Result<Playlist> NeteaseLibrary::get_playlist(const PlaylistRef &ref,
                                              const CallContext &ctx) {
  using R = Result<Playlist>;
  if (ref.entity_type != EntityType::Playlist ||
      ref.library_id != library_id_) {
    return R::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid playlist reference"));
  }
  const auto id = api_enhanced::parse_platform_id(ref.entity_id);
  if (!id.ok()) {
    return R::failure(id.error());
  }

  std::shared_ptr<api_enhanced::NeteaseClient> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = client_;
  }
  if (client == nullptr) {
    return R::failure(
        Error::make(ErrorCode::ProviderUnavailable, "netease client is closed"));
  }

  auto detail = client->playlist(id.value(), ctx);
  if (!detail.ok()) {
    return R::failure(detail.error());
  }
  if (detail.value().id <= 0) {
    return R::failure(
        Error::make(ErrorCode::NotFound, "netease playlist not found"));
  }

  Playlist playlist;
  playlist.ref = ref;
  playlist.name = detail.value().name;
  playlist.description = detail.value().description;

  // /playlist/track/all 不返回 more，只能按 trackCount 收敛。
  // 500/页，最多 20 页（10000 曲）作为防跑飞的硬上限。
  constexpr int kPageSize = 500;
  constexpr int kMaxPages = 20;
  const std::int64_t total = detail.value().track_count;
  for (int page_index = 0; page_index < kMaxPages; ++page_index) {
    const std::int64_t offset = static_cast<std::int64_t>(page_index) * kPageSize;
    if (total > 0 && offset >= total) {
      break;
    }
    auto fetched = client->playlist_tracks(id.value(), kPageSize,
                                           static_cast<int>(offset), ctx);
    if (!fetched.ok()) {
      // 已经拿到的部分仍然有价值；首屏失败才整体失败。
      if (playlist.tracks.empty()) {
        return R::failure(fetched.error());
      }
      break;
    }
    if (fetched.value().items.empty()) {
      break;
    }
    for (const auto &song : fetched.value().items) {
      if (song.id <= 0) {
        continue;
      }
      auto track_ref = api_enhanced::make_ref(EntityType::Track, song.id);
      if (!track_ref.ok()) {
        continue;
      }
      playlist.tracks.push_back(std::move(track_ref).value());
    }
    if (static_cast<int>(fetched.value().items.size()) < kPageSize) {
      break;
    }
  }

  return R::success(std::move(playlist));
}

Result<std::shared_ptr<NeteaseLibrary>>
make_netease_library(const std::string &library_id,
                     const std::string &display_name,
                     const api_enhanced::NeteaseTransportOptions &transport,
                     const api_enhanced::NeteaseClientOptions &client_options,
                     const std::string &uid) {
  auto http = api_enhanced::make_http_transport(transport);
  if (http == nullptr) {
    return Result<std::shared_ptr<NeteaseLibrary>>::failure(
        Error::make(ErrorCode::InvalidRequest,
                    "netease api_base is not a valid http(s) url"));
  }

  auto client =
      std::make_shared<api_enhanced::NeteaseClient>(std::move(http),
                                                    client_options);
  auto library = std::make_shared<NeteaseLibrary>(library_id, display_name,
                                                  std::move(client), uid);
  library->initialize();
  library->probe_health(CallContext{});

  return Result<std::shared_ptr<NeteaseLibrary>>::success(std::move(library));
}

} // namespace media_library
