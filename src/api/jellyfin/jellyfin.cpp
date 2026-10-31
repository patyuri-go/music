#include "jellyfin.h"

#include "config.h"
#include "database.h"
#include "db.h"
#include "dto.h"
#include "http_util.h"
#include "ids.h"
#include "schema.h"
#include "storage.h"
#include "api/playlist/playlist.h"
#include "media_library/media_runtime.h"
#include "media_library/snapshot.h"
#include "stream_http.h"

#include "crow/app.h"
#include "crow/http_request.h"
#include "crow/http_response.h"
#include "crow/json.h"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace jellyfin {
namespace {

namespace fs = std::filesystem;

constexpr const char *kVersion = "10.10.7";

// 门面不再直接持有本地库对象：元数据一律经 MediaRouter/ArtworkService，
// 「库内部 int id」与本地快照这类确实属于某个实现的读操作走组合根的兼容访问器。
// TODO(N6): 跨库聚合完成后删除 snapshot()。
media_library::CatalogSnapshot snapshot() {
  media_library::CatalogSnapshot snap;
  media_library::compat_snapshot(snap);
  return snap;
}

// 目录实体的对外 id 就是 SourceRef 的规范字符串（见 ids.h 的 itemId）。
// 它不依赖数据库自增主键：重扫后不变，远端 provider 也能给出合法 id。
using IdPair = std::pair<std::string, std::string>;

std::vector<IdPair> artist_pairs_of(
    const std::vector<media_library::ArtistLink> &links) {
  std::vector<IdPair> out;
  out.reserve(links.size());
  for (const auto &link : links) {
    out.emplace_back(link.name, itemId(link.ref));
  }
  return out;
}

std::vector<IdPair> album_artist_pairs_of(
    const std::vector<media_library::AlbumArtistLink> &links) {
  std::vector<IdPair> out;
  out.reserve(links.size());
  for (const auto &link : links) {
    out.emplace_back(link.name, itemId(link.ref));
  }
  return out;
}

// 路径参数由 Crow 原样给出（未解码），解析前先做百分号解码。
std::optional<media_library::SourceRef>
parse_path_ref(const std::string &text) {
  return parseItemId(http_util::urlDecode(text));
}



std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string trimCopy(const std::string &s) {
  const std::size_t b = s.find_first_not_of(" \t\r\n");
  const std::size_t e = s.find_last_not_of(" \t\r\n");
  return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
}

// 同时按原样与全小写注册路由，兼容 ASP.NET 的大小写不敏感路由。
struct RouteReg {
  crow::SimpleApp &app;
  std::string path;
  std::function<void(crow::DynamicRule &)> apply_methods;
  RouteReg(crow::SimpleApp &a, std::string p) : app(a), path(std::move(p)) {}
  template <typename... M> RouteReg &methods(M... m) {
    apply_methods = [m...](crow::DynamicRule &r) { r.methods(m...); };
    return *this;
  }
  template <typename F> void operator()(F f) {
    // 路由匹配已由 crow::case_insensitive_routing() 全局开启大小写不敏感，
    // 这里只需按原样注册一次。
    auto &r = app.route_dynamic(path);
    if (apply_methods) apply_methods(r);
    r(f);
  }
};

// 查询参数读取：Jellyfin 客户端使用 PascalCase（如 ParentId），
// 而 Crow 的 url_params.get() 大小写敏感，这里统一做大小写不敏感匹配。
std::string qs(const crow::request &req, const char *key) {
  const std::string want = lower(std::string(key));
  for (const auto &k : req.url_params.keys()) {
    if (lower(k) == want) {
      const char *v = req.url_params.get(k);
      return v ? std::string(v) : std::string();
    }
  }
  return std::string();
}

int qsInt(const crow::request &req, const char *key, int def) {
  const std::string v = qs(req, key);
  if (v.empty()) {
    return def;
  }
  try {
    return std::stoi(v);
  } catch (...) {
    return def;
  }
}

bool qsBool(const crow::request &req, const char *key, bool def) {
  const std::string v = qs(req, key);
  if (v.empty()) {
    return def;
  }
  return v == "true" || v == "1" || v == "True";
}

bool listHas(const std::string &csv, const std::string &needle) {
  if (csv.empty()) {
    return false;
  }
  const std::string hay = lower(csv);
  const std::string want = lower(needle);
  std::string::size_type pos = 0;
  while (pos <= hay.size()) {
    const auto comma = hay.find(',', pos);
    const std::string token =
        hay.substr(pos, comma == std::string::npos ? std::string::npos
                                                   : comma - pos);
    if (token == want) {
      return true;
    }
    if (comma == std::string::npos) {
      break;
    }
    pos = comma + 1;
  }
  return false;
}

crow::response json(int code, const crow::json::wvalue &v) {
  return http_util::jsonResponse(code, v.dump());
}

crow::response emptyList() {
  crow::json::wvalue arr;
  arr = crow::json::wvalue::list{};
  return json(200, arr);
}

// ------------------------------------------------------------------ 排序/分页

struct Sortable {
  std::string sortKey;
  std::string addedAt;
  int year = 0;
  std::string albumKey;
  std::string artistKey;
  int playCount = 0;
  std::string lastPlayed;
  int discNo = 1;
  int trackNo = 0;
  crow::json::wvalue dto;
};

namespace {

enum class SortField {
  SortKey,
  AlbumKey,
  ArtistKey,
  AddedAt,
  Year,
  PlayCount,
  LastPlayed,
  DiscNo,
  TrackNo
};

int compareSortField(const Sortable &a, const Sortable &b, SortField f) {
  switch (f) {
  case SortField::SortKey: {
    const std::string x = lower(a.sortKey);
    const std::string y = lower(b.sortKey);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  case SortField::AlbumKey: {
    const std::string x = lower(a.albumKey);
    const std::string y = lower(b.albumKey);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  case SortField::ArtistKey: {
    const std::string x = lower(a.artistKey);
    const std::string y = lower(b.artistKey);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  case SortField::AddedAt:
    return a.addedAt < b.addedAt ? -1 : (a.addedAt > b.addedAt ? 1 : 0);
  case SortField::Year:
    return a.year < b.year ? -1 : (a.year > b.year ? 1 : 0);
  case SortField::PlayCount:
    return a.playCount < b.playCount ? -1 : (a.playCount > b.playCount ? 1 : 0);
  case SortField::LastPlayed:
    return a.lastPlayed < b.lastPlayed
               ? -1
               : (a.lastPlayed > b.lastPlayed ? 1 : 0);
  case SortField::DiscNo:
    return a.discNo < b.discNo ? -1 : (a.discNo > b.discNo ? 1 : 0);
  case SortField::TrackNo:
    return a.trackNo < b.trackNo ? -1 : (a.trackNo > b.trackNo ? 1 : 0);
  }
  return 0;
}

} // namespace

void sortItems(std::vector<Sortable> &items, const std::string &sortBy,
               const std::string &order) {
  // Jellyfin sends a comma separated list of sort keys. Apply every key we
  // recognise, in order, as a tie-breaker chain. Unknown keys are ignored so
  // that the order produced by the database query is preserved.
  std::vector<SortField> fields;
  bool random = false;
  std::size_t start = 0;
  while (start <= sortBy.size()) {
    const std::size_t comma = sortBy.find(',', start);
    std::string tok =
        (comma == std::string::npos) ? sortBy.substr(start)
                                     : sortBy.substr(start, comma - start);
    tok = lower(trimCopy(tok));
    if (tok == "random") {
      random = true;
    } else if (tok == "name" || tok == "sortname") {
      fields.push_back(SortField::SortKey);
    } else if (tok == "album") {
      fields.push_back(SortField::AlbumKey);
    } else if (tok == "albumartist" || tok == "artist") {
      fields.push_back(SortField::ArtistKey);
    } else if (tok == "datecreated" || tok == "datelastcontentadded" ||
               tok == "datelastmediaadded") {
      fields.push_back(SortField::AddedAt);
    } else if (tok == "productionyear") {
      fields.push_back(SortField::Year);
    } else if (tok == "playcount") {
      fields.push_back(SortField::PlayCount);
    } else if (tok == "dateplayed" || tok == "datelastplayed") {
      fields.push_back(SortField::LastPlayed);
    } else if (tok == "parentindexnumber") {
      fields.push_back(SortField::DiscNo);
    } else if (tok == "indexnumber") {
      fields.push_back(SortField::TrackNo);
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  if (random) {
    std::mt19937 rng(std::random_device{}());
    std::shuffle(items.begin(), items.end(), rng);
    return;
  }
  if (fields.empty()) {
    return;
  }
  std::stable_sort(items.begin(), items.end(),
                   [&fields](const Sortable &a, const Sortable &b) {
                     for (const SortField f : fields) {
                       const int c = compareSortField(a, b, f);
                       if (c != 0) {
                         return c < 0;
                       }
                     }
                     return false;
                   });
  if (order == "Descending") {
    std::reverse(items.begin(), items.end());
  }
}

crow::response respondItems(std::vector<Sortable> &items,
                            const std::string &sortBy,
                            const std::string &order, int startIndex,
                            int limit) {
  sortItems(items, sortBy, order);
  const int total = static_cast<int>(items.size());
  int from = startIndex < 0 ? 0 : startIndex;
  if (from > total) {
    from = total;
  }
  int to = total;
  if (limit > 0 && from + limit < total) {
    to = from + limit;
  }
  std::vector<crow::json::wvalue> out;
  for (int i = from; i < to; ++i) {
    out.push_back(std::move(items[i].dto));
  }
  return json(200, queryResult(std::move(out), total, from));
}

// ------------------------------------------------- 快照到 Jellyfin DTO 的映射

// 快照只携带统一领域模型；兼容层负责把 SourceRef 映射成对外 ID。
std::vector<std::pair<std::string, int>> genre_pairs(
    const media_library::CatalogSnapshot &snap, int track_id) {
  std::vector<std::pair<std::string, int>> out;
  const auto it = snap.track_genres.find(track_id);
  if (it == snap.track_genres.end()) {
    return out;
  }
  for (const auto &genre : it->second) {
    out.emplace_back(genre.name, genre.internal_id);
  }
  return out;
}

Sortable itemForTrack(const media_library::CatalogSnapshot &snap,
                      std::size_t index) {
  const auto &track = snap.tracks[index];
  const int track_id = snap.track_internal_ids[index];
  Sortable s;
  s.sortKey = track.title;
  s.addedAt = track.user_data.added_at;
  s.year = track.year.value_or(0);
  s.albumKey = track.album ? track.album->title : std::string();
  const auto trackArtists = artist_pairs_of(track.artists);
  s.artistKey =
      trackArtists.empty() ? std::string() : trackArtists.front().first;
  s.playCount = track.user_data.play_count;
  s.lastPlayed = track.user_data.last_played;
  s.discNo = track.disc_no.value_or(0);
  s.trackNo = track.track_no.value_or(0);
  std::string filePath;
  const auto path_it = snap.track_paths.find(track_id);
  if (path_it != snap.track_paths.end()) {
    filePath = path_it->second;
  }
  s.dto = trackItem(track, itemId(track.ref),
                    track.album ? itemId(track.album->ref) : std::string(),
                    s.albumKey, filePath, trackArtists,
                    album_artist_pairs_of(track.album_artists),
                    genre_pairs(snap, track_id));
  return s;
}

Sortable itemForAlbum(const media_library::CatalogSnapshot &snap,
                      std::size_t index) {
  const auto &album = snap.albums[index];
  const int album_id = snap.album_internal_ids[index];
  Sortable s;
  s.sortKey = album.title;
  s.year = album.year.value_or(0);
  int count = 0;
  const auto count_it = snap.album_track_count.find(album_id);
  if (count_it != snap.album_track_count.end()) {
    count = count_it->second;
  }
  long long durationMs = 0;
  const auto duration_it = snap.album_duration_ms.find(album_id);
  if (duration_it != snap.album_duration_ms.end()) {
    durationMs = duration_it->second;
  }
  s.dto = albumItem(album, itemId(album.ref),
                    album_artist_pairs_of(album.artists), count,
                    album.artwork ? "1" : "", durationMs * 10000LL);
  return s;
}

Sortable itemForArtist(const media_library::CatalogSnapshot &snap,
                       std::size_t index) {
  const auto &artist = snap.artists[index];
  const int artist_id = snap.artist_internal_ids[index];
  Sortable s;
  s.sortKey = artist.sort_name.empty() ? artist.name : artist.sort_name;
  int count = 0;
  const auto it = snap.artist_album_count.find(artist_id);
  if (it != snap.artist_album_count.end()) {
    count = it->second;
  }
  s.dto = artistItem(artist, itemId(artist.ref), count,
                     artist.artwork ? "1" : "");
  return s;
}

Sortable itemForGenre(const media_library::GenreEntry &genre) {
  Sortable s;
  s.sortKey = genre.name;
  s.dto = genreItem(genre.internal_id, genre.name);
  return s;
}

Sortable makePlaylist(const playlist::Playlist &p) {
  int matched = 0;
  long long totalMs = 0;
  for (const auto &song : p.songs) {
    if (song.matched) {
      ++matched;
      totalMs += song.duration_ms;
    }
  }
  Sortable s;
  s.sortKey = p.name;
  crow::json::wvalue it;
  it["Name"] = p.name;
  it["ServerId"] = serverId();
  it["Id"] = p.id;
  it["Type"] = "Playlist";
  it["MediaType"] = "Audio";
  it["IsFolder"] = true;
  it["ChildCount"] = matched;
  if (totalMs > 0) {
    it["RunTimeTicks"] = totalMs * 10000LL;
    it["CumulativeRunTimeTicks"] = totalMs * 10000LL;
  }
  s.dto = std::move(it);
  return s;
}

// 歌单 DTO（与 Jellyfin GetPlaylist 一致）。
crow::json::wvalue playlistDto(const playlist::Playlist &p) {
  crow::json::wvalue::list ids;
  int matched = 0;
  long long totalMs = 0;
  for (const auto &song : p.songs) {
    if (song.matched) {
      // playlist 子系统内部仍以库内 int id 存曲目；对外一律输出稳定 id。
      const auto ref = media_library::compat_ref_for(
          media_library::EntityType::Track, song.track_id);
      if (ref) {
        ids.emplace_back(itemId(*ref));
        ++matched;
        totalMs += song.duration_ms;
      }
    }
  }
  crow::json::wvalue r;
  r["Name"] = p.name;
  r["ServerId"] = serverId();
  r["Id"] = p.id;
  r["Type"] = "Playlist";
  r["MediaType"] = "Audio";
  r["IsFolder"] = true;
  r["ItemIds"] = crow::json::wvalue(ids);
  r["ChildCount"] = matched;
  if (totalMs > 0) {
    r["RunTimeTicks"] = totalMs * 10000LL;
    r["CumulativeRunTimeTicks"] = totalMs * 10000LL;
  }
  return r;
}

// ---------------------------------------------------------------- 视图/系统

crow::json::wvalue musicView() {
  crow::json::wvalue v;
  v["Name"] = "Music";
  v["ServerId"] = serverId();
  v["Id"] = musicViewId();
  v["Type"] = "UserView";
  v["CollectionType"] = "music";
  v["IsFolder"] = true;
  return v;
}

crow::json::wvalue playlistsView() {
  crow::json::wvalue v;
  v["Name"] = "Playlists";
  v["ServerId"] = serverId();
  v["Id"] = playlistsViewId();
  v["Type"] = "UserView";
  v["CollectionType"] = "playlists";
  v["IsFolder"] = true;
  return v;
}

crow::response userViews() {
  std::vector<crow::json::wvalue> items;
  items.push_back(musicView());
  items.push_back(playlistsView());
  return json(200, queryResult(std::move(items), 2, 0));
}

crow::json::wvalue publicSystemInfo() {
  crow::json::wvalue r;
  r["LocalAddress"] = "127.0.0.1";
  r["ServerName"] = serverName();
  r["Version"] = kVersion;
  r["ProductName"] = "Jellyfin Server";
  r["OperatingSystem"] = "Linux";
  r["Id"] = serverId();
  r["StartupWizardCompleted"] = true;
  return r;
}

crow::json::wvalue systemInfo() {
  crow::json::wvalue r = publicSystemInfo();
  r["OperatingSystemDisplayName"] = "Linux";
  r["PackageName"] = "";
  r["HasPendingRestart"] = false;
  r["IsShuttingDown"] = false;
  r["SupportsLibraryMonitor"] = false;
  r["WebSocketPortNumber"] = 0;
  r["CanSelfRestart"] = false;
  r["CanLaunchWebBrowser"] = false;
  r["ProgramDataPath"] = "";
  r["WebPath"] = "/web";
  r["ItemsByNamePath"] = "";
  r["CachePath"] = "";
  r["LogPath"] = "";
  r["InternalMetadataPath"] = "";
  r["TranscodingTempPath"] = "";
  r["HasUpdateAvailable"] = false;
  r["EncoderLocation"] = "";
  r["SystemArchitecture"] = "X64";
  r["CompletedInstallations"] =
      crow::json::wvalue(crow::json::wvalue::list{});
  r["CastReceiverApplications"] =
      crow::json::wvalue(crow::json::wvalue::list{});
  return r;
}

crow::json::wvalue brandingOptions() {
  crow::json::wvalue r;
  r["LoginDisclaimer"] = "";
  r["CustomCss"] = "";
  r["SplashscreenEnabled"] = false;
  return r;
}

crow::json::wvalue displayPreferences(const std::string &id) {
  crow::json::wvalue r;
  r["Id"] = id;
  r["ViewType"] = "";
  r["SortBy"] = "";
  r["IndexBy"] = "";
  r["RememberIndexing"] = false;
  r["PrimaryImageHeight"] = 250;
  r["PrimaryImageWidth"] = 250;
  crow::json::wvalue prefs;
  prefs["client"] = "emby";
  r["CustomPrefs"] = std::move(prefs);
  r["ScrollDirection"] = "Horizontal";
  r["ShowBackdrop"] = true;
  r["RememberSorting"] = false;
  r["SortOrder"] = "Ascending";
  r["ShowSidebar"] = false;
  r["Client"] = "";
  return r;
}

// ------------------------------------------------------------------- 核心接口

std::vector<std::string> splitCsv(const std::string &s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t comma = s.find(',', start);
    std::string part = (comma == std::string::npos) ? s.substr(start)
                                                    : s.substr(start, comma - start);
    if (!part.empty()) out.push_back(part);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

crow::response handleItems(const crow::request &req) {
  const std::string parent = qs(req, "parentId");
  const std::string include = qs(req, "includeItemTypes");
  const std::string search = lower(qs(req, "searchTerm"));
  const int startIndex = qsInt(req, "startIndex", 0);
  const int limit = qsInt(req, "limit", 0);
  const std::string sortBy = qs(req, "sortBy");
  const std::string order = qs(req, "sortOrder");
  const bool isPlayed = qsBool(req, "isPlayed", false);
  const std::vector<std::string> wantGenres = splitCsv(qs(req, "genres"));
  const std::vector<std::string> wantGenreIds = splitCsv(qs(req, "genreIds"));
  const std::vector<std::string> wantYears = splitCsv(qs(req, "years"));
  const std::vector<std::string> wantAlbumIds = splitCsv(qs(req, "albumIds"));
  const std::vector<std::string> wantArtistIds = splitCsv(qs(req, "artistIds"));
  const std::vector<std::string> wantAlbumArtistIds =
      splitCsv(qs(req, "albumArtistIds"));
  const std::vector<std::string> wantContribArtists =
      splitCsv(qs(req, "contributingArtistIds"));
  const std::string filters = qs(req, "filters");
  const bool wantPlayed = filters.find("IsPlayed") != std::string::npos;
  const bool wantUnplayed = filters.find("IsUnplayed") != std::string::npos;
  const std::string nameStartsWith = lower(qs(req, "nameStartsWith"));

  // 目录实体 id 是 SourceRef 规范串；genre / view 仍是非目录 id 形态。
  const std::optional<media_library::SourceRef> parentRef =
      parent.empty() ? std::nullopt : parseItemId(parent);
  int genreParentId = 0;
  {
    int gk = 0;
    int gid = 0;
    if (parseId(parent, gk, gid) && gk == kKindGenre) {
      genreParentId = gid;
    }
  }
  const bool hasParent = parentRef.has_value();
  const bool hasGenreParent = genreParentId != 0;

  const media_library::CatalogSnapshot snap = snapshot();

  enum class Mode { Tracks, Albums, Artists, Genres, Playlists };
  Mode mode = Mode::Albums;
  bool explicitMode = false;
  if (listHas(include, "MusicArtist")) {
    mode = Mode::Artists;
    explicitMode = true;
  } else if (listHas(include, "MusicAlbum")) {
    mode = Mode::Albums;
    explicitMode = true;
  } else if (listHas(include, "MusicGenre")) {
    mode = Mode::Genres;
    explicitMode = true;
  } else if (listHas(include, "Playlist")) {
    mode = Mode::Playlists;
    explicitMode = true;
  } else if (listHas(include, "Audio") || listHas(include, "MusicVideo")) {
    mode = Mode::Tracks;
    explicitMode = true;
  }
  if (!explicitMode) {
    if (parentRef.has_value() &&
        parentRef->entity_type == media_library::EntityType::Album) {
      mode = Mode::Tracks;
    } else if (parentRef.has_value() &&
               (parentRef->entity_type == media_library::EntityType::Artist ||
                parentRef->entity_type ==
                    media_library::EntityType::AlbumArtist)) {
      mode = Mode::Albums;
    } else if (hasGenreParent) {
      mode = Mode::Tracks;
    }
  }

  // artist / album_artist 是两种不同的引用：比较时显式转换，避免混用。
  const auto as_artist = [](const media_library::SourceRef &ref) {
    return ref.entity_type == media_library::EntityType::AlbumArtist
               ? media_library::as_artist_ref(ref)
               : ref;
  };
  const auto as_album_artist = [](const media_library::SourceRef &ref) {
    return ref.entity_type == media_library::EntityType::Artist
               ? media_library::as_album_artist_ref(ref)
               : ref;
  };
  const auto has_artist =
      [&](const std::vector<media_library::ArtistLink> &links,
          const std::vector<media_library::SourceRef> &refs) {
        for (const auto &link : links) {
          for (const auto &ref : refs) {
            if (link.ref == as_artist(ref)) {
              return true;
            }
          }
        }
        return false;
      };
  const auto has_album_artist =
      [&](const std::vector<media_library::AlbumArtistLink> &links,
          const std::vector<media_library::SourceRef> &refs) {
        for (const auto &link : links) {
          for (const auto &ref : refs) {
            if (link.ref == as_album_artist(ref)) {
              return true;
            }
          }
        }
        return false;
      };
  const auto parse_refs = [](const std::vector<std::string> &values,
                             std::optional<media_library::EntityType> type) {
    std::vector<media_library::SourceRef> out;
    for (const auto &text : values) {
      const auto ref = parseItemId(text);
      if (!ref.has_value()) {
        continue;
      }
      if (type.has_value() && ref->entity_type != *type) {
        continue;
      }
      out.push_back(*ref);
    }
    return out;
  };
  const auto parse_genre_ids = [](const std::vector<std::string> &values) {
    std::vector<int> out;
    for (const auto &text : values) {
      int k = 0;
      int v = 0;
      if (parseId(text, k, v) && k == kKindGenre) {
        out.push_back(v);
      }
    }
    return out;
  };
  const auto contains_ref =
      [](const std::vector<media_library::SourceRef> &refs,
         const media_library::SourceRef &ref) {
        return std::find(refs.begin(), refs.end(), ref) != refs.end();
      };
  const auto contains_id = [](const std::vector<int> &ids, int id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
  };

  std::vector<Sortable> items;
  if (mode == Mode::Tracks) {
    const auto album_ids =
        parse_refs(wantAlbumIds, media_library::EntityType::Album);
    const auto artist_ids =
        parse_refs(wantArtistIds, media_library::EntityType::Artist);
    const auto contrib_ids =
        parse_refs(wantContribArtists, media_library::EntityType::Artist);
    const auto album_artist_ids = parse_refs(
        wantAlbumArtistIds, media_library::EntityType::AlbumArtist);
    const std::vector<int> genre_ids = parse_genre_ids(wantGenreIds);
    for (std::size_t i = 0; i < snap.tracks.size(); ++i) {
      const auto &t = snap.tracks[i];
      const int track_id = snap.track_internal_ids[i];
      if (hasParent &&
          parentRef->entity_type == media_library::EntityType::Album) {
        if (!t.album || !(t.album->ref == *parentRef)) {
          continue;
        }
      } else if (hasParent && parentRef->entity_type ==
                                  media_library::EntityType::Artist) {
        if (!has_artist(t.artists, {*parentRef}) &&
            !has_album_artist(t.album_artists, {*parentRef})) {
          continue;
        }
      } else if (hasGenreParent) {
        bool matched = false;
        const auto git = snap.track_genres.find(track_id);
        if (git != snap.track_genres.end()) {
          for (const auto &genre : git->second) {
            if (genre.internal_id == genreParentId) {
              matched = true;
              break;
            }
          }
        }
        if (!matched) {
          continue;
        }
      }
      if (!search.empty() && lower(t.title).find(search) == std::string::npos) {
        continue;
      }
      const int play_count = t.user_data.play_count;
      if (isPlayed && play_count <= 0) {
        continue;
      }
      if (!nameStartsWith.empty() &&
          lower(t.title).rfind(nameStartsWith, 0) != 0) {
        continue;
      }
      if (wantPlayed && play_count <= 0) {
        continue;
      }
      if (wantUnplayed && play_count > 0) {
        continue;
      }
      if (!album_ids.empty()) {
        if (!t.album || !contains_ref(album_ids, t.album->ref)) {
          continue;
        }
      }
      if (!artist_ids.empty() || !contrib_ids.empty()) {
        const auto &list = artist_ids.empty() ? contrib_ids : artist_ids;
        if (!has_artist(t.artists, list) &&
            !has_album_artist(t.album_artists, list)) {
          continue;
        }
      }
      if (!album_artist_ids.empty()) {
        if (!has_album_artist(t.album_artists, album_artist_ids)) {
          continue;
        }
      }
      if (!wantYears.empty()) {
        const std::string year = std::to_string(t.year.value_or(0));
        if (std::find(wantYears.begin(), wantYears.end(), year) ==
            wantYears.end()) {
          continue;
        }
      }
      if (!wantGenres.empty() || !genre_ids.empty()) {
        bool ok = false;
        const auto git = snap.track_genres.find(track_id);
        if (git != snap.track_genres.end()) {
          for (const auto &genre : git->second) {
            for (const auto &want : wantGenres) {
              if (lower(genre.name) == lower(want)) {
                ok = true;
                break;
              }
            }
            if (ok) {
              break;
            }
            if (contains_id(genre_ids, genre.internal_id)) {
              ok = true;
              break;
            }
          }
        }
        if (!ok) {
          continue;
        }
      }
      items.push_back(itemForTrack(snap, i));
    }
  } else if (mode == Mode::Albums) {
    const auto artist_ids =
        parse_refs(wantArtistIds, media_library::EntityType::Artist);
    const auto album_artist_ids = parse_refs(
        wantAlbumArtistIds, media_library::EntityType::AlbumArtist);
    for (std::size_t i = 0; i < snap.albums.size(); ++i) {
      const auto &a = snap.albums[i];
      if (hasParent && (parentRef->entity_type ==
                            media_library::EntityType::Artist ||
                        parentRef->entity_type ==
                            media_library::EntityType::AlbumArtist)) {
        if (!has_album_artist(a.artists, {*parentRef})) {
          continue;
        }
      }
      if (!search.empty() && lower(a.title).find(search) == std::string::npos) {
        continue;
      }
      if (!artist_ids.empty() || !album_artist_ids.empty()) {
        const auto &list =
            artist_ids.empty() ? album_artist_ids : artist_ids;
        if (!has_album_artist(a.artists, list)) {
          continue;
        }
      }
      items.push_back(itemForAlbum(snap, i));
    }
  } else if (mode == Mode::Artists) {
    for (std::size_t i = 0; i < snap.artists.size(); ++i) {
      const auto &a = snap.artists[i];
      if (!search.empty() && lower(a.name).find(search) == std::string::npos) {
        continue;
      }
      items.push_back(itemForArtist(snap, i));
    }
  } else if (mode == Mode::Genres) {
    for (const auto &g : snap.genres) {
      if (!search.empty() && lower(g.name).find(search) == std::string::npos) {
        continue;
      }
      items.push_back(itemForGenre(g));
    }
  } else {
    for (const auto &p : playlist::list()) {
      if (!search.empty() && lower(p.name).find(search) == std::string::npos) {
        continue;
      }
      items.push_back(makePlaylist(p));
    }
  }
  return respondItems(items, sortBy, order, startIndex, limit);
}

crow::response handleLatest(const crow::request &req) {
  const std::string include = qs(req, "includeItemTypes");
  const int limit = qsInt(req, "limit", 20);
  const int cap = limit > 0 ? limit : 20;
  const media_library::CatalogSnapshot snap = snapshot();
  std::vector<crow::json::wvalue> out;
  if (listHas(include, "Audio")) {
    std::vector<std::size_t> order(snap.tracks.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) {
                       const auto &x = snap.tracks[a].user_data.added_at;
                       const auto &y = snap.tracks[b].user_data.added_at;
                       if (x != y) {
                         return x > y;
                       }
                       return snap.track_internal_ids[a] >
                              snap.track_internal_ids[b];
                     });
    for (std::size_t i = 0; i < order.size() && static_cast<int>(i) < cap;
         ++i) {
      out.push_back(itemForTrack(snap, order[i]).dto);
    }
  } else {
    std::vector<std::size_t> order(snap.albums.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) {
                       return snap.album_internal_ids[a] >
                              snap.album_internal_ids[b];
                     });
    for (std::size_t i = 0; i < order.size() && static_cast<int>(i) < cap;
         ++i) {
      out.push_back(itemForAlbum(snap, order[i]).dto);
    }
  }
  crow::json::wvalue arr;
  arr = std::move(out);
  return json(200, arr);
}

crow::response handleItem(const std::string &idstr) {
  {
    playlist::Playlist p;
    if (playlist::get(idstr, p)) {
      return json(200, playlistDto(p));
    }
  }
  const std::string decoded = http_util::urlDecode(idstr);
  if (decoded == musicViewId() || decoded == playlistsViewId()) {
    return json(200, musicView());
  }

  const auto ref = parseItemId(decoded);
  if (ref.has_value()) {
    if (ref->entity_type == media_library::EntityType::Track) {
      const auto result = media_library::media_router().get_track(*ref, {});
      if (result.ok()) {
        const auto &t = result.value().value;
        // Path 是 Jellyfin 的文件系统字段；locator 不出进程，这里留空（5.4.4）。
        const std::string filePath;
        std::vector<std::pair<std::string, int>> genres;
        if (!t.genres.empty()) {
          const media_library::CatalogSnapshot snap = snapshot();
          for (const auto &name : t.genres) {
            int genreId = 0;
            for (const auto &g : snap.genres) {
              if (g.name == name) {
                genreId = g.internal_id;
                break;
              }
            }
            genres.emplace_back(name, genreId);
          }
        }
        return json(200, trackItem(
                             t, itemId(t.ref),
                             t.album ? itemId(t.album->ref) : std::string(),
                             t.album ? t.album->title : std::string(),
                             filePath, artist_pairs_of(t.artists),
                             album_artist_pairs_of(t.album_artists), genres));
      }
    } else if (ref->entity_type == media_library::EntityType::Album) {
      const auto result = media_library::media_router().get_album(*ref, {});
      if (result.ok()) {
        const auto &a = result.value().value;
        int count = 0;
        long long runtimeMs = 0;
        const auto tracks =
            media_library::media_router().list_album_tracks(*ref, {std::nullopt, 1000000}, {});
        if (tracks.ok()) {
          count = static_cast<int>(tracks.value().value.items.size());
          for (const auto &track : tracks.value().value.items) {
            runtimeMs += track.duration_ms.value_or(0);
          }
        }
        return json(200, albumItem(a, itemId(a.ref),
                                   album_artist_pairs_of(a.artists), count,
                                   a.artwork ? "1" : "",
                                   runtimeMs * 10000LL));
      }
    } else if (ref->entity_type == media_library::EntityType::Artist ||
               ref->entity_type == media_library::EntityType::AlbumArtist) {
      const media_library::ArtistRef artist_ref =
          ref->entity_type == media_library::EntityType::AlbumArtist
              ? media_library::as_artist_ref(*ref)
              : *ref;
      const auto result = media_library::media_router().get_artist(artist_ref, {});
      if (result.ok()) {
        const auto &a = result.value().value;
        int count = 0;
        const auto albums = media_library::media_router().list_artist_albums(
            artist_ref, {std::nullopt, 1000000}, {});
        if (albums.ok()) {
          count = static_cast<int>(albums.value().value.items.size());
        }
        return json(200, artistItem(a, itemId(a.ref), count,
                                    a.artwork ? "1" : ""));
      }
    }
  }

  int kind = 0;
  int id = 0;
  if (parseId(decoded, kind, id) && kind == kKindGenre) {
    const media_library::CatalogSnapshot snap = snapshot();
    for (const auto &g : snap.genres) {
      if (g.internal_id == id) {
        return json(200, genreItem(id, g.name));
      }
    }
  }
  return http_util::jsonResponse(404, "{}");
}

// 封面字节统一从 ArtworkService 出：选源交给 MediaRouter，locator 不外泄。
crow::response handleImage(const crow::request &req, const std::string &idstr) {
  const bool head_only = req.method == crow::HTTPMethod::HEAD;

  // 歌单封面：每次请求随机返回歌单内一首已匹配曲目的专辑封面。
  playlist::Playlist p;
  if (playlist::get(idstr, p)) {
    std::vector<media_library::SourceRef> tracks;
    for (const auto &s : p.songs) {
      if (!s.matched) {
        continue;
      }
      // TODO(N6): 跨库聚合后歌单曲目本身就用 SourceRef。
      if (auto trackRef = media_library::compat_ref_for(
              media_library::EntityType::Track, s.track_id)) {
        tracks.push_back(std::move(*trackRef));
      }
    }
    if (tracks.empty()) {
      return http_util::jsonResponse(404, "{}");
    }
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<std::size_t> dist(0, tracks.size() - 1);
    media_library::ArtworkRef artwork;
    artwork.owner = std::move(tracks[dist(rng)]);
    return stream_http::serve_artwork(req, artwork, head_only);
  }

  const auto ref = parse_path_ref(idstr);
  if (!ref.has_value() ||
      (ref->entity_type != media_library::EntityType::Track &&
       ref->entity_type != media_library::EntityType::Album &&
       ref->entity_type != media_library::EntityType::Artist &&
       ref->entity_type != media_library::EntityType::AlbumArtist)) {
    return http_util::jsonResponse(404, "{}");
  }
  media_library::ArtworkRef artwork;
  artwork.owner = ref->entity_type == media_library::EntityType::AlbumArtist
                      ? media_library::as_artist_ref(*ref)
                      : *ref;
  return stream_http::serve_artwork(req, artwork, head_only);
}

crow::response handleStream(const crow::request &req,
                            const std::string &idstr) {
  const auto ref = parse_path_ref(idstr);
  if (!ref.has_value() ||
      ref->entity_type != media_library::EntityType::Track) {
    return http_util::jsonResponse(404, "{}");
  }
  // 音频字节统一从播放会话出：选源交给 MediaRouter，locator 不外泄。
  return stream_http::serve_track(req, *ref, req.method == crow::HTTPMethod::HEAD);
}

/// 解析单个 LRC 时间标签（mm:ss、mm:ss.xx 或 mm:ss:xx）为毫秒。
bool parseLrcTimestamp(const std::string &s, long long &ms) {
  // Split the timestamp by ':'. Supported forms:
  //   [mm:ss]  [mm:ss.xx]  [mm:ss:xx]  [h:mm:ss]  [h:mm:ss.xx]
  std::vector<std::string> parts;
  {
    std::string cur;
    for (const char ch : s) {
      if (ch == ':') {
        parts.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(ch);
      }
    }
    parts.push_back(cur);
  }
  if (parts.size() < 2 || parts.size() > 4) {
    return false;
  }
  for (const auto &p : parts) {
    if (p.empty()) {
      return false;
    }
  }
  // Optional fractional part on the last component (after '.' or ',').
  long long frac_ms = 0;
  {
    std::string &last = parts.back();
    const std::size_t dot = last.find_first_of(".,");
    if (dot != std::string::npos) {
      std::string digits;
      for (std::size_t i = dot + 1; i < last.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(last[i]))) {
          digits.push_back(last[i]);
        }
      }
      if (!digits.empty()) {
        frac_ms = static_cast<long long>(
            std::atof(("0." + digits).c_str()) * 1000.0 + 0.5);
      }
      last = last.substr(0, dot);
    }
  }
  const auto toInt = [](const std::string &p, long long &out) -> bool {
    if (p.empty()) {
      return false;
    }
    out = 0;
    for (const char c : p) {
      if (c < '0' || c > '9') {
        return false;
      }
      out = out * 10 + (c - '0');
    }
    return true;
  };
  long long a = 0;
  long long b = 0;
  long long c = 0;
  if (parts.size() == 2) {
    // mm:ss
    if (!toInt(parts[0], a) || !toInt(parts[1], b)) {
      return false;
    }
    ms = (a * 60 + b) * 1000 + frac_ms;
    return true;
  }
  if (!toInt(parts[0], a) || !toInt(parts[1], b) || !toInt(parts[2], c)) {
    return false;
  }
  if (parts.size() == 3) {
    if (parts[0].size() <= 1) {
      // h:mm:ss
      ms = (a * 3600 + b * 60 + c) * 1000 + frac_ms;
    } else {
      // mm:ss:cc (legacy centisecond form)
      ms = (a * 60 + b) * 1000 + c * 10 + frac_ms;
    }
    return true;
  }
  // h:mm:ss:cc
  long long d = 0;
  if (!toInt(parts[3], d)) {
    return false;
  }
  ms = (a * 3600 + b * 60 + c) * 1000 + d * 10 + frac_ms;
  return true;
}

crow::json::wvalue buildLyricDto(const std::string &title,
                                 long long duration_ms,
                                 const std::string &lyrics, bool synced,
                                 const std::string &artist,
                                 const std::string &album) {
  crow::json::wvalue r;
  crow::json::wvalue::list lines;
  std::string by;
  std::string version;
  long long offset_ms = 0;

  std::size_t pos = 0;
  while (pos <= lyrics.size()) {
    const std::size_t nl = lyrics.find('\n', pos);
    std::string raw = lyrics.substr(
        pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? lyrics.size() + 1 : nl + 1;
    if (!raw.empty() && raw.back() == '\r') {
      raw.pop_back();
    }

    std::vector<long long> starts;
    std::size_t i = 0;
    while (i < raw.size() && raw[i] == '[') {
      const std::size_t close = raw.find(']', i);
      if (close == std::string::npos) {
        break;
      }
      const std::string tag = raw.substr(i + 1, close - i - 1);
      long long ms = 0;
      if (parseLrcTimestamp(tag, ms)) {
        starts.push_back(ms);
      } else {
        const auto c2 = tag.find(':');
        if (c2 != std::string::npos) {
          const std::string key = lower(tag.substr(0, c2));
          const std::string val = tag.substr(c2 + 1);
          if (key == "by") {
            by = val;
          } else if (key == "ve") {
            version = val;
          } else if (key == "offset") {
            offset_ms = std::atoll(val.c_str());
          }
        }
      }
      i = close + 1;
    }

    const std::string text = raw.substr(i);
    if (text.empty()) {
      continue;
    }
    if (starts.empty()) {
      crow::json::wvalue line;
      line["Text"] = text;
      lines.push_back(std::move(line));
    } else {
      for (const long long ms : starts) {
        crow::json::wvalue line;
        line["Text"] = text;
        line["Start"] = (ms + offset_ms) * 10000LL;
        lines.push_back(std::move(line));
      }
    }
  }

  crow::json::wvalue meta;
  meta["Title"] = title;
  if (!artist.empty()) {
    meta["Artist"] = artist;
  }
  if (!album.empty()) {
    meta["Album"] = album;
  }
  if (!by.empty()) {
    meta["By"] = by;
  }
  if (!version.empty()) {
    meta["Version"] = version;
  }
  meta["Length"] = duration_ms * 10000LL;
  meta["IsSynced"] = synced;
  r["Metadata"] = std::move(meta);
  r["Lyrics"] = crow::json::wvalue(lines);
  return r;
}

crow::response handleLyrics(const std::string &idstr) {
  const auto ref = parse_path_ref(idstr);
  if (!ref.has_value() ||
      ref->entity_type != media_library::EntityType::Track) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto lyricsResult = media_library::media_router().resolve_lyrics(*ref, {});
  const auto trackResult = media_library::media_router().resolve_track(*ref, {});
  if (!lyricsResult.ok() || !trackResult.ok()) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto &lyrics = lyricsResult.value().value;
  const std::string plain = lyrics.plain_text.value_or(std::string());
  if (plain.empty()) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto &track = trackResult.value().value;
  const std::string artist =
      track.artists.empty() ? std::string() : track.artists.front().name;
  const std::string album =
      track.album ? track.album->title : std::string();
  return json(200, buildLyricDto(track.title, track.duration_ms.value_or(0),
                                 plain,
                                 lyrics.mode != media_library::LyricsMode::Unsynced,
                                 artist, album));
}

crow::response handleArtists(const crow::request &req,
                             bool album_only = false) {
  const std::string search = lower(qs(req, "searchTerm"));
  const int startIndex = qsInt(req, "startIndex", 0);
  const int limit = qsInt(req, "limit", 0);
  const std::string sortBy = qs(req, "sortBy");
  const std::string order = qs(req, "sortOrder");
  const media_library::CatalogSnapshot snap = snapshot();
  std::vector<Sortable> items;
  for (std::size_t i = 0; i < snap.artists.size(); ++i) {
    const auto &a = snap.artists[i];
    // AlbumArtists must not include plain (track-only) artists.
    if (album_only &&
        snap.artist_album_count.find(snap.artist_internal_ids[i]) ==
            snap.artist_album_count.end()) {
      continue;
    }
    if (!search.empty() && lower(a.name).find(search) == std::string::npos) {
      continue;
    }
    items.push_back(itemForArtist(snap, i));
  }
  return respondItems(items, sortBy, order, startIndex, limit);
}

crow::response handleGenres(const crow::request &req) {
  const std::string search = lower(qs(req, "searchTerm"));
  const int startIndex = qsInt(req, "startIndex", 0);
  const int limit = qsInt(req, "limit", 0);
  const media_library::CatalogSnapshot snap = snapshot();
  std::vector<Sortable> items;
  for (const auto &g : snap.genres) {
    if (!search.empty() && lower(g.name).find(search) == std::string::npos) {
      continue;
    }
    items.push_back(itemForGenre(g));
  }
  return respondItems(items, "", "Ascending", startIndex, limit);
}

crow::response handleMusicGenreByName(const std::string &name) {
  const std::string want = lower(name);
  const media_library::CatalogSnapshot snap = snapshot();
  for (const auto &g : snap.genres) {
    if (lower(g.name) == want) {
      return json(200, genreItem(g.internal_id, g.name));
    }
  }
  return http_util::jsonResponse(404, "{}");
}

// ------------------------------------------------------------ 只读扩展接口

// 播放信息：返回单个 MediaSource，供客户端直接播放原始文件。
crow::response handlePlaybackInfo(const crow::request &, const std::string &idstr) {
  const auto ref = parse_path_ref(idstr);
  if (!ref.has_value() ||
      ref->entity_type != media_library::EntityType::Track) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto service = media_library::playback_service_ptr();
  if (service == nullptr) {
    return stream_http::error_response(media_library::Error::make(
        media_library::ErrorCode::ProviderUnavailable,
        "media runtime is not initialized"));
  }
  // 用会话拿一次摘要，随后立刻关闭：这里只需要「能不能播、多大、多久」。
  auto session = service->open_playback(*ref, media_library::CallContext{});
  if (!session.ok()) {
    return stream_http::error_response(session.error());
  }
  const media_library::Track &t = session.value().track.value;
  const media_library::PlaybackSourceSummary &summary =
      session.value().stream.value;
  service->close(session.value().session_id);

  const long long durationMs = summary.duration_ms;
  const int bitrateKbps = summary.bitrate_kbps;
  const std::string url =
      "/Audio/" + idstr + "/stream?static=true&mediaSourceId=" + idstr;
  const long long bitrate_bps = static_cast<long long>(bitrateKbps) * 1000LL;
  crow::json::wvalue src;
  src["Id"] = idstr;
  src["Name"] = t.title;
  // locator 是进程内定位符，绝不能出现在响应里（5.4.4）。Path 用相对播放地址。
  src["Path"] = url;
  src["Container"] = t.audio.format;
  src["Size"] = t.audio.size_bytes.value_or(0);
  src["Bitrate"] = bitrate_bps;
  src["RunTimeTicks"] = durationMs * 10000LL;
  src["SupportsDirectPlay"] = true;
  src["SupportsDirectStream"] = true;
  src["SupportsTranscoding"] = false;
  src["IsRemote"] = false;
  src["Type"] = "Default";
  src["DirectStreamUrl"] = url;
  src["TranscodingUrl"] = url;

  crow::json::wvalue stream;
  stream["Type"] = "Audio";
  stream["Codec"] = t.audio.format;
  stream["Index"] = 0;
  stream["IsDefault"] = true;
  stream["Channels"] = t.audio.channels.value_or(0);
  stream["SampleRate"] = t.audio.sample_rate.value_or(0);
  stream["BitRate"] = bitrate_bps;
  crow::json::wvalue::list streams;
  streams.emplace_back(std::move(stream));
  src["MediaStreams"] = crow::json::wvalue(streams);

  crow::json::wvalue::list sources;
  sources.emplace_back(std::move(src));
  crow::json::wvalue r;
  r["MediaSources"] = crow::json::wvalue(sources);
  r["PlaySessionId"] = "musicd-session";
  return json(200, r);
}

// 仅支持 Primary 图片，其它图片类型返回 404。
crow::response handleImageType(const crow::request &req, const std::string &idstr,
                               const std::string &imageType, const std::string &) {
  if (lower(imageType) != "primary") {
    return http_util::jsonResponse(404, "{}");
  }
  return handleImage(req, idstr);
}

// 相似条目：同专辑 + 同流派的其它曲目。
crow::response handleSimilar(const std::string &idstr) {
  std::vector<Sortable> items;
  const auto ref = parse_path_ref(idstr);
  if (ref.has_value() && ref->entity_type == media_library::EntityType::Track) {
    const media_library::CatalogSnapshot snap = snapshot();
    std::size_t origin = snap.tracks.size();
    for (std::size_t i = 0; i < snap.tracks.size(); ++i) {
      if (snap.tracks[i].ref == *ref) {
        origin = i;
        break;
      }
    }
    if (origin < snap.tracks.size()) {
      const auto &origin_track = snap.tracks[origin];
      const auto origin_internal = media_library::compat_internal_id_of(origin_track.ref);
      std::unordered_set<int> want_genres;
      if (origin_internal) {
        const auto genres = snap.track_genres.find(*origin_internal);
        if (genres != snap.track_genres.end()) {
          for (const auto &g : genres->second) {
            want_genres.insert(g.internal_id);
          }
        }
      }
      std::unordered_set<std::string> seen{media_library::to_string(*ref)};
      constexpr std::size_t kLimit = 20;
      auto add_track = [&](std::size_t index) {
        if (items.size() >= kLimit) {
          return;
        }
        const std::string key = media_library::to_string(snap.tracks[index].ref);
        if (!seen.insert(key).second) {
          return;
        }
        items.push_back(itemForTrack(snap, index));
      };
      if (origin_track.album.has_value()) {
        const media_library::AlbumRef album = origin_track.album->ref;
        for (std::size_t i = 0; i < snap.tracks.size() && items.size() < kLimit;
             ++i) {
          const auto &other = snap.tracks[i];
          if (other.album && other.album->ref == album) {
            add_track(i);
          }
        }
      }
      for (std::size_t i = 0; i < snap.tracks.size() && items.size() < kLimit;
           ++i) {
        const int track_id = snap.track_internal_ids[i];
        const auto genres_it = snap.track_genres.find(track_id);
        if (genres_it == snap.track_genres.end()) {
          continue;
        }
        bool shares = false;
        for (const auto &g : genres_it->second) {
          if (want_genres.count(g.internal_id) != 0) {
            shares = true;
            break;
          }
        }
        if (shares) {
          add_track(i);
        }
      }
    }
  }
  return respondItems(items, "", "Ascending", 0, 0);
}

// 下载原始文件（与 /Items/<id>/File 同义）。
crow::response handleDownload(const crow::request &req, const std::string &idstr) {
  const auto ref = parse_path_ref(idstr);
  if (!ref.has_value() ||
      ref->entity_type != media_library::EntityType::Track) {
    return http_util::jsonResponse(404, "{}");
  }
  // 下载与在线播放走同一条会话路径：字节来源一致，Range 语义一致。
  return stream_http::serve_track(req, *ref, req.method == crow::HTTPMethod::HEAD);
}

// 搜索建议：曲目 / 专辑 / 艺术家。
crow::response handleSearchHints(const crow::request &req) {
  const std::string term = qs(req, "searchTerm");
  const int limit = qsInt(req, "limit", 0);
  const int cap = limit > 0 ? limit : 20;
  const std::string lterm = lower(term);
  crow::json::wvalue::list hints;
  auto push = [&](const std::string &id, const std::string &name, const char *type) {
    if ((int)hints.size() >= cap) return;
    crow::json::wvalue h;
    h["ItemId"] = id;
    h["Id"] = id;
    h["Name"] = name;
    h["Type"] = type;
    h["MediaType"] = "Audio";
    hints.emplace_back(std::move(h));
  };
  if (!term.empty()) {
    const media_library::CatalogSnapshot snap = snapshot();
    std::vector<std::size_t> track_hits;
    for (std::size_t i = 0; i < snap.tracks.size(); ++i) {
      if (lower(snap.tracks[i].title).find(lterm) != std::string::npos) {
        track_hits.push_back(i);
      }
    }
    std::stable_sort(track_hits.begin(), track_hits.end(),
                     [&](std::size_t a, std::size_t b) {
                       return snap.tracks[a].user_data.added_at >
                              snap.tracks[b].user_data.added_at;
                     });
    for (const std::size_t i : track_hits) {
      if ((int)hints.size() >= cap) break;
      const auto &t = snap.tracks[i];
      push(itemId(t.ref),
           t.title.empty() ? ("Track " + t.ref.entity_id.substr(0, 8))
                           : t.title,
           "Audio");
    }
    for (std::size_t i = 0; i < snap.albums.size() && (int)hints.size() < cap;
         ++i) {
      const auto &a = snap.albums[i];
      if (lower(a.title).find(lterm) != std::string::npos) {
        push(itemId(a.ref), a.title, "MusicAlbum");
      }
    }
    for (std::size_t i = 0; i < snap.artists.size() && (int)hints.size() < cap;
         ++i) {
      const auto &a = snap.artists[i];
      if (lower(a.name).find(lterm) != std::string::npos) {
        push(itemId(a.ref), a.name, "MusicArtist");
      }
    }
  }
  crow::json::wvalue r;
  r["SearchHints"] = crow::json::wvalue(hints);
  r["TotalRecordCount"] = static_cast<int>(hints.size());
  return json(200, r);
}

// 过滤选项：真实流派与年份。
crow::response handleFilters(const crow::request &) {
  const media_library::CatalogSnapshot snap = snapshot();
  crow::json::wvalue::list genres;
  for (const auto &g : snap.genres) genres.emplace_back(g.name);
  std::vector<int> years;
  for (const auto &a : snap.albums) {
    if (a.year && *a.year > 0) years.push_back(*a.year);
  }
  std::sort(years.begin(), years.end());
  years.erase(std::unique(years.begin(), years.end()), years.end());
  crow::json::wvalue::list yr;
  for (int y : years) yr.emplace_back(y);
  crow::json::wvalue r;
  r["Genres"] = crow::json::wvalue(genres);
  r["Tags"] = crow::json::wvalue(crow::json::wvalue::list{});
  r["OfficialRatings"] = crow::json::wvalue(crow::json::wvalue::list{});
  r["Years"] = crow::json::wvalue(yr);
  return json(200, r);
}

// 按稳定 id（或名字）取单个艺术家。
crow::response handleArtistByName(const std::string &raw) {
  const media_library::CatalogSnapshot snap = snapshot();
  std::optional<std::size_t> index;
  const std::string decoded = http_util::urlDecode(raw);
  if (const auto ref = parseItemId(decoded)) {
    const media_library::SourceRef artist_ref =
        ref->entity_type == media_library::EntityType::AlbumArtist
            ? media_library::as_artist_ref(*ref)
            : *ref;
    for (std::size_t i = 0; i < snap.artists.size(); ++i) {
      if (snap.artists[i].ref == artist_ref) {
        index = i;
        break;
      }
    }
  } else {
    for (std::size_t i = 0; i < snap.artists.size(); ++i) {
      if (snap.artists[i].name == decoded) {
        index = i;
        break;
      }
    }
  }
  if (!index) {
    return http_util::jsonResponse(404, "{}");
  }
  return json(200, itemForArtist(snap, *index).dto);
}

// 相似艺术家：与本艺术家共享流派的其它艺术家。
crow::response handleArtistSimilar(const std::string &raw) {
  const media_library::CatalogSnapshot snap = snapshot();
  const std::string decoded = http_util::urlDecode(raw);
  std::optional<media_library::SourceRef> artist_ref;
  if (const auto ref = parseItemId(decoded)) {
    media_library::SourceRef candidate =
        ref->entity_type == media_library::EntityType::AlbumArtist
            ? media_library::as_artist_ref(*ref)
            : *ref;
    if (candidate.entity_type == media_library::EntityType::Artist) {
      artist_ref = std::move(candidate);
    }
  } else {
    for (const auto &artist : snap.artists) {
      if (artist.name == decoded) {
        artist_ref = artist.ref;
        break;
      }
    }
  }
  if (!artist_ref.has_value()) {
    return http_util::jsonResponse(404, "{}");
  }

  std::unordered_set<int> genre_ids;
  for (std::size_t i = 0; i < snap.tracks.size(); ++i) {
    bool by_artist = false;
    for (const auto &link : snap.tracks[i].artists) {
      if (link.ref == *artist_ref) {
        by_artist = true;
        break;
      }
    }
    if (!by_artist) {
      continue;
    }
    const auto genres = snap.track_genres.find(snap.track_internal_ids[i]);
    if (genres == snap.track_genres.end()) {
      continue;
    }
    for (const auto &g : genres->second) {
      genre_ids.insert(g.internal_id);
    }
  }

  std::vector<media_library::SourceRef> similar;
  std::unordered_set<std::string> seen{media_library::to_string(*artist_ref)};
  constexpr std::size_t kLimit = 20;
  for (std::size_t i = 0; i < snap.tracks.size() && similar.size() < kLimit;
       ++i) {
    const auto genres = snap.track_genres.find(snap.track_internal_ids[i]);
    if (genres == snap.track_genres.end()) {
      continue;
    }
    bool shares = false;
    for (const auto &g : genres->second) {
      if (genre_ids.count(g.internal_id) != 0) {
        shares = true;
        break;
      }
    }
    if (!shares) {
      continue;
    }
    for (const auto &link : snap.tracks[i].artists) {
      if (!seen.insert(media_library::to_string(link.ref)).second) {
        continue;
      }
      similar.push_back(link.ref);
      if (similar.size() >= kLimit) {
        break;
      }
    }
  }

  std::vector<Sortable> items;
  for (const auto &wanted : similar) {
    for (std::size_t i = 0; i < snap.artists.size(); ++i) {
      if (snap.artists[i].ref == wanted) {
        items.push_back(itemForArtist(snap, i));
        break;
      }
    }
  }
  return respondItems(items, "", "Ascending", 0, 0);
}

// 解析逗号分隔的稳定 track id 列表为库内 int（遗留 playlist 子系统只认 int）。
std::vector<int> parseTrackIds(const std::string &csv) {
  std::vector<int> ids;
  for (const auto &tok : splitCsv(csv)) {
    const auto ref = parseItemId(tok);
    if (!ref.has_value() ||
        ref->entity_type != media_library::EntityType::Track) {
      continue;
    }
    const auto internal = media_library::compat_internal_id_of(*ref);
    if (internal.has_value()) {
      ids.push_back(*internal);
    }
  }
  return ids;
}

// 大小写不敏感地收集某 query 参数的全部出现（支持重复键）。
std::vector<std::string> qsList(const crow::request &req, const char *key) {
  std::vector<std::string> out;
  const std::string want = lower(std::string(key));
  for (const auto &k : req.url_params.keys()) {
    if (lower(k) != want) {
      continue;
    }
    for (char *v : req.url_params.get_list(k, false)) {
      if (v != nullptr) {
        out.emplace_back(v);
      }
    }
    for (char *v : req.url_params.get_list(k, true)) {
      if (v != nullptr) {
        out.emplace_back(v);
      }
    }
  }
  return out;
}

// 合并某 query 参数的全部出现（逗号连接）。
std::string qsJoined(const crow::request &req, const char *key) {
  std::string joined;
  for (const auto &v : qsList(req, key)) {
    if (!joined.empty()) {
      joined += ',';
    }
    joined += v;
  }
  return joined;
}

// 从 JSON body 的某个稳定 id 数组字段追加 track id。
void appendBodyIds(const crow::json::rvalue &body, const char *name,
                   std::vector<int> &out) {
  if (!body || !body.has(name) ||
      body[name].t() != crow::json::type::List) {
    return;
  }
  for (const auto &x : body[name]) {
    if (x.t() != crow::json::type::String) {
      continue;
    }
    const auto ref = parseItemId(std::string(x.s()));
    if (!ref.has_value() ||
        ref->entity_type != media_library::EntityType::Track) {
      continue;
    }
    const auto internal = media_library::compat_internal_id_of(*ref);
    if (internal.has_value()) {
      out.push_back(*internal);
    }
  }
}

// 歌单列表。
crow::response handlePlaylists() {
  std::vector<crow::json::wvalue> items;
  for (const auto &p : playlist::list()) {
    items.push_back(makePlaylist(p).dto);
  }
  const int total = static_cast<int>(items.size());
  return json(200, queryResult(std::move(items), total, 0));
}

// 歌单详情。
crow::response handlePlaylistById(const std::string &idstr) {
  playlist::Playlist p;
  if (!playlist::get(idstr, p)) {
    return http_util::jsonResponse(404, "{}");
  }
  return json(200, playlistDto(p));
}

// 歌单内的曲目。
crow::response handlePlaylistItems(const std::string &idstr) {
  playlist::Playlist p;
  if (!playlist::get(idstr, p)) {
    return http_util::jsonResponse(404, "{}");
  }
  const media_library::CatalogSnapshot snap = snapshot();
  std::unordered_map<int, std::size_t> index_of;
  for (std::size_t i = 0; i < snap.track_internal_ids.size(); ++i) {
    index_of.emplace(snap.track_internal_ids[i], i);
  }
  std::vector<Sortable> items;
  for (const auto &s : p.songs) {
    if (!s.matched) {
      continue;
    }
    const auto it = index_of.find(s.track_id);
    if (it == index_of.end()) {
      continue;
    }
    Sortable item = itemForTrack(snap, it->second);
    // PlaylistItemId 对外也用稳定 id；内部 int 只在 playlist 子系统里流转。
    if (const auto ref = media_library::compat_ref_for(
            media_library::EntityType::Track, s.track_id)) {
      item.dto["PlaylistItemId"] = itemId(*ref);
    }
    item.dto["CanDelete"] = true;
    items.push_back(std::move(item));
  }
  return respondItems(items, "", "Ascending", 0, 0);
}

// 新建歌单（name/ids 可来自 query 或 JSON body）。
crow::response handleCreatePlaylist(const crow::request &req) {
  const auto body = crow::json::load(req.body);
  std::string name = qs(req, "name");
  if (name.empty() && body && body.has("Name")) {
    name = body["Name"].s();
  }
  if (name.empty()) {
    return http_util::jsonResponse(400, "{}");
  }
  playlist::Playlist p = playlist::create(name);
  std::vector<int> ids = parseTrackIds(qsJoined(req, "ids"));
  appendBodyIds(body, "Ids", ids);
  std::string err;
  for (int tid : ids) {
    playlist::addTrack(p.id, tid, err);
  }
  crow::json::wvalue r;
  r["Id"] = p.id;
  return json(200, r);
}

// 向歌单添加曲目（ids 可来自 query 或 JSON body）。
crow::response handleAddToPlaylist(const crow::request &req,
                                   const std::string &idstr) {
  playlist::Playlist p;
  if (!playlist::get(idstr, p)) {
    return http_util::jsonResponse(404, "{}");
  }
  std::vector<int> ids = parseTrackIds(qsJoined(req, "ids"));
  const auto body = crow::json::load(req.body);
  appendBodyIds(body, "Ids", ids);
  std::string err;
  for (int tid : ids) {
    playlist::addTrack(idstr, tid, err);
  }
  return crow::response(204);
}

// 从歌单移除曲目（entryIds / ids 均按曲目 GUID 处理，可来自 query 或 body）。
crow::response handleRemoveFromPlaylist(const crow::request &req,
                                        const std::string &idstr) {
  playlist::Playlist p;
  if (!playlist::get(idstr, p)) {
    return http_util::jsonResponse(404, "{}");
  }
  std::vector<int> wanted = parseTrackIds(qsJoined(req, "entryIds"));
  if (wanted.empty()) {
    wanted = parseTrackIds(qsJoined(req, "ids"));
  }
  const auto body = crow::json::load(req.body);
  appendBodyIds(body, "EntryIds", wanted);
  appendBodyIds(body, "Ids", wanted);
  for (int tid : wanted) {
    playlist::removeTrack(idstr, tid);
  }
  return crow::response(204);
}

// 更新歌单：改名与/或替换曲目（name/ids 可来自 query 或 body）。
crow::response handleUpdatePlaylist(const crow::request &req,
                                    const std::string &idstr) {
  playlist::Playlist p;
  if (!playlist::get(idstr, p)) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto body = crow::json::load(req.body);
  std::string name = qs(req, "name");
  if (name.empty() && body && body.has("Name")) {
    name = body["Name"].s();
  }
  if (!name.empty()) {
    playlist::rename(idstr, name);
  }
  std::vector<int> ids = parseTrackIds(qsJoined(req, "ids"));
  appendBodyIds(body, "Ids", ids);
  if (!ids.empty()) {
    playlist::clear(idstr);
    std::string err;
    for (int tid : ids) {
      playlist::addTrack(idstr, tid, err);
    }
  }
  return crow::response(204);
}

// 删除歌单。
crow::response handleDeletePlaylist(const std::string &idstr) {
  if (!playlist::remove(idstr)) {
    return http_util::jsonResponse(404, "{}");
  }
  return crow::response(204);
}

} // namespace

#define JF_ROUTE(app, url) RouteReg((app), (url))

void run(const config::Config &cfg) {
  setServerName(cfg.jellyfin_server_name);

  crow::SimpleApp app;

  // 系统
  JF_ROUTE(app, "/System/Info/Public")
  ([] { return json(200, publicSystemInfo()); });
  JF_ROUTE(app, "/System/Info")([] { return json(200, systemInfo()); });
  JF_ROUTE(app, "/System/Ping")
  ([] { return crow::response(200, "Jellyfin Server"); });
  JF_ROUTE(app, "/System/Ping").methods(crow::HTTPMethod::Post)
  ([](const crow::request &) { return crow::response(200, "Jellyfin Server"); });
  JF_ROUTE(app, "/System/Endpoint")([] {
    crow::json::wvalue r;
    r["IsLocal"] = true;
    r["IsInNetwork"] = true;
    return json(200, r);
  });

  // 用户/鉴权
  JF_ROUTE(app, "/Users/Public")([] {
    std::vector<crow::json::wvalue> items;
    items.push_back(userDto());
    crow::json::wvalue arr;
    arr = std::move(items);
    return json(200, arr);
  });
  JF_ROUTE(app, "/Users/AuthenticateByName").methods(crow::HTTPMethod::Post)
  ([](const crow::request &req) {
    std::string client;
    auto body = crow::json::load(req.body);
    if (body && body.has("Username")) {
      client = body["Username"].s();
    }
    crow::json::wvalue r;
    r["User"] = userDto();
    r["SessionInfo"] = sessionInfo(client, "");
    r["AccessToken"] = "musicd-token";
    r["ServerId"] = serverId();
    return json(200, r);
  });
  JF_ROUTE(app, "/Users/Me")([] { return json(200, userDto()); });
  JF_ROUTE(app, "/Users/<string>")
  ([](const crow::request &, std::string) { return json(200, userDto()); });

  // 视图
  JF_ROUTE(app, "/UserViews")([] { return userViews(); });
  JF_ROUTE(app, "/Users/<string>/Views")
  ([](const crow::request &, std::string) { return userViews(); });

  // 条目
  JF_ROUTE(app, "/Items/Latest")
  ([](const crow::request &req) { return handleLatest(req); });
  JF_ROUTE(app, "/Items/Filters")
  ([](const crow::request &req) { return handleFilters(req); });
  JF_ROUTE(app, "/Users/<string>/Items/Latest")
  ([](const crow::request &req, std::string) { return handleLatest(req); });
  JF_ROUTE(app, "/Items")
  ([](const crow::request &req) { return handleItems(req); });
  JF_ROUTE(app, "/Users/<string>/Items")
  ([](const crow::request &req, std::string) { return handleItems(req); });
  JF_ROUTE(app, "/Users/<string>/Items/<string>")
  ([](const crow::request &, std::string, std::string itemId) {
    return handleItem(itemId);
  });
  JF_ROUTE(app, "/Items/<string>/PlaybackInfo")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post)
  ([](const crow::request &req, std::string itemId) { return handlePlaybackInfo(req, itemId); });
  JF_ROUTE(app, "/Items/<string>/Images/<string>/<string>")
  ([](const crow::request &req, std::string itemId, std::string imageType,
      std::string imageIndex) {
    return handleImageType(req, itemId, imageType, imageIndex);
  });
  JF_ROUTE(app, "/Items/<string>/Images/<string>")
  ([](const crow::request &req, std::string itemId, std::string imageType) {
    return handleImageType(req, itemId, imageType, "");
  });
  JF_ROUTE(app, "/Items/<string>/Download")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Head)
  ([](const crow::request &req, std::string itemId) { return handleDownload(req, itemId); });
  JF_ROUTE(app, "/Items/<string>/File")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Head)
  ([](const crow::request &req, std::string itemId) { return handleDownload(req, itemId); });
  JF_ROUTE(app, "/Items/<string>/Similar")
  ([](const crow::request &, std::string itemId) { return handleSimilar(itemId); });
  JF_ROUTE(app, "/Items/<string>")
  ([](const crow::request &, std::string itemId) { return handleItem(itemId); });
  // 通用删除项（官方 DeleteItem）：目前仅支持删除歌单，其他类型返回 404。
  JF_ROUTE(app, "/Items/<string>").methods(crow::HTTPMethod::Delete)
  ([](const crow::request &, std::string itemId) {
    return handleDeletePlaylist(itemId);
  });

  // 流媒体
  JF_ROUTE(app, "/Audio/<string>/Lyrics")
  ([](const crow::request &, std::string itemId) { return handleLyrics(itemId); });
  JF_ROUTE(app, "/Audio/<string>/<string>")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Head)
  ([](const crow::request &req, std::string itemId, std::string) {
    return handleStream(req, itemId);
  });

  // 艺术家/流派
  JF_ROUTE(app, "/Artists")
  ([](const crow::request &req) { return handleArtists(req); });
  JF_ROUTE(app, "/Artists/AlbumArtists")
  ([](const crow::request &req) { return handleArtists(req, true); });
  JF_ROUTE(app, "/Artists/<string>/Images/<string>")
  ([](const crow::request &req, std::string name, std::string imageType) {
    return handleImageType(req, name, imageType, "");
  });
  JF_ROUTE(app, "/Artists/<string>/Similar")
  ([](const crow::request &, std::string name) { return handleArtistSimilar(name); });
  JF_ROUTE(app, "/Artists/<string>")
  ([](const crow::request &, std::string name) {
    return handleArtistByName(name);
  });
  JF_ROUTE(app, "/MusicGenres")
  ([](const crow::request &req) { return handleGenres(req); });
  JF_ROUTE(app, "/MusicGenres/<string>")
  ([](const crow::request &, std::string name) {
    return handleMusicGenreByName(http_util::urlDecode(name));
  });
  JF_ROUTE(app, "/Genres")
  ([](const crow::request &req) { return handleGenres(req); });
  JF_ROUTE(app, "/Genres/<string>")
  ([](const crow::request &, std::string name) {
    return handleMusicGenreByName(http_util::urlDecode(name));
  });
  JF_ROUTE(app, "/Studios")
  ([](const crow::request &) {
    std::vector<crow::json::wvalue> empty;
    return json(200, queryResult(std::move(empty), 0, 0));
  });
  JF_ROUTE(app, "/Studios/<string>")
  ([](const crow::request &, std::string) {
    std::vector<crow::json::wvalue> empty;
    return json(200, queryResult(std::move(empty), 0, 0));
  });

  // 播放会话
  JF_ROUTE(app, "/Sessions")([] { return emptyList(); });
  JF_ROUTE(app, "/Sessions/Playing").methods(crow::HTTPMethod::Post)
  ([](const crow::request &) { return crow::response(204); });
  JF_ROUTE(app, "/Sessions/Playing/Progress").methods(crow::HTTPMethod::Post)
  ([](const crow::request &) { return crow::response(204); });
  JF_ROUTE(app, "/Sessions/Playing/Stopped").methods(crow::HTTPMethod::Post)
  ([](const crow::request &) { return crow::response(204); });

  // 收藏
  JF_ROUTE(app, "/UserFavoriteItems/<string>").methods(crow::HTTPMethod::Post)
  ([](const crow::request &, std::string) {
    crow::json::wvalue r;
    r["IsFavorite"] = true;
    r["PlayCount"] = 0;
    r["Played"] = false;
    r["Key"] = "";
    return json(200, r);
  });
  JF_ROUTE(app, "/UserFavoriteItems/<string>")
      .methods(crow::HTTPMethod::Delete)
  ([](const crow::request &, std::string) {
    crow::json::wvalue r;
    r["IsFavorite"] = false;
    r["PlayCount"] = 0;
    r["Played"] = false;
    r["Key"] = "";
    return json(200, r);
  });

  // 占位/默认值
  JF_ROUTE(app, "/Branding/Configuration")
  ([] { return json(200, brandingOptions()); });
  JF_ROUTE(app, "/DisplayPreferences/<string>")
  ([](const crow::request &, std::string id) {
    return json(200, displayPreferences(id));
  });
  JF_ROUTE(app, "/DisplayPreferences/<string>").methods(crow::HTTPMethod::Post)
  ([](const crow::request &, std::string) { return crow::response(204); });
  JF_ROUTE(app, "/ScheduledTasks")([] { return emptyList(); });
  JF_ROUTE(app, "/Plugins")([] { return emptyList(); });
  JF_ROUTE(app, "/Search/Hints")
  ([](const crow::request &req) { return handleSearchHints(req); });
  JF_ROUTE(app, "/Shows/NextUp")([] {
    std::vector<crow::json::wvalue> empty;
    return json(200, queryResult(std::move(empty), 0, 0));
  });
  JF_ROUTE(app, "/UserItems/Resume")([] {
    std::vector<crow::json::wvalue> empty;
    return json(200, queryResult(std::move(empty), 0, 0));
  });
  JF_ROUTE(app, "/Playlists")([] { return handlePlaylists(); });
  JF_ROUTE(app, "/Playlists").methods(crow::HTTPMethod::Post)
  ([](const crow::request &req) { return handleCreatePlaylist(req); });
  JF_ROUTE(app, "/Playlists/<string>")
  ([](const crow::request &, std::string id) { return handlePlaylistById(id); });
  JF_ROUTE(app, "/Playlists/<string>").methods(crow::HTTPMethod::Post)
  ([](const crow::request &req, std::string id) {
    return handleUpdatePlaylist(req, id);
  });
  JF_ROUTE(app, "/Playlists/<string>").methods(crow::HTTPMethod::Delete)
  ([](const crow::request &, std::string id) { return handleDeletePlaylist(id); });
  JF_ROUTE(app, "/Playlists/<string>/Items")
  ([](const crow::request &, std::string id) { return handlePlaylistItems(id); });
  JF_ROUTE(app, "/Playlists/<string>/Items").methods(crow::HTTPMethod::Post)
  ([](const crow::request &req, std::string id) {
    return handleAddToPlaylist(req, id);
  });
  JF_ROUTE(app, "/Playlists/<string>/Items").methods(crow::HTTPMethod::Delete)
  ([](const crow::request &req, std::string id) {
    return handleRemoveFromPlaylist(req, id);
  });
  JF_ROUTE(app, "/Localization/Options")([] {
    crow::json::wvalue arr;
    arr = crow::json::wvalue::list{};
    return json(200, arr);
  });
  JF_ROUTE(app, "/Users/<string>/GroupingOptions")
  ([](const crow::request &, std::string) {
    crow::json::wvalue r;
    r["GroupingOptions"] = crow::json::wvalue::list{};
    return json(200, r);
  });
  JF_ROUTE(app, "/QuickConnect/Enabled")([] {
    crow::json::wvalue r;
    r = false;
    return json(200, r);
  });
  JF_ROUTE(app, "/System/Configuration")([] {
    crow::json::wvalue r;
    return json(200, r);
  });
  JF_ROUTE(app, "/Users/<string>/Configuration")
  ([](const crow::request &, std::string) {
    crow::json::wvalue r;
    return json(200, r);
  });

  // 路由大小写不敏感：Crow 的字面量路由默认大小写敏感，这里开启全局开关，
  // 使路由匹配忽略大小写（参数值保持原样）。Jellyfin 客户端会以各种大小写请求。
  crow::case_insensitive_routing() = true;

  app.loglevel(crow::LogLevel::Info);
  app.port(cfg.jellyfin_port).multithreaded().concurrency(32).run();
}

} // namespace jellyfin
