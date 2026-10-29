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
#include "media_library/library_runtime.h"
#include "media_library/local_library.h"
#include "media_library/snapshot.h"

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

// Library 对象由 main() 创建；9000 与 5666 共用同一个实例。
media_library::LocalLibrary &library() { return media_library::library(); }

int compat_id_of(const media_library::SourceRef &ref) {
  const auto internal = library().internal_id_of(ref);
  return internal ? *internal : 0;
}

std::vector<std::pair<std::string, int>> artist_pairs_of(
    const std::vector<media_library::ArtistLink> &links) {
  std::vector<std::pair<std::string, int>> out;
  for (const auto &link : links) {
    out.emplace_back(link.name, compat_id_of(link.ref));
  }
  return out;
}

std::vector<std::pair<std::string, int>> album_artist_pairs_of(
    const std::vector<media_library::AlbumArtistLink> &links) {
  std::vector<std::pair<std::string, int>> out;
  for (const auto &link : links) {
    out.emplace_back(link.name, compat_id_of(link.ref));
  }
  return out;
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

// 快照只携带统一领域模型；兼容层负责把 SourceRef 映射回对外 ID。
std::vector<int> artist_compat_ids(
    const std::vector<media_library::ArtistLink> &links) {
  std::vector<int> out;
  for (const auto &link : links) {
    out.push_back(compat_id_of(link.ref));
  }
  return out;
}

std::vector<int> album_artist_compat_ids(
    const std::vector<media_library::AlbumArtistLink> &links) {
  std::vector<int> out;
  for (const auto &link : links) {
    out.push_back(compat_id_of(link.ref));
  }
  return out;
}

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
  s.dto = trackItem(track, track_id, track.album ? compat_id_of(track.album->ref) : 0,
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
  s.dto = albumItem(album, album_id, album_artist_pairs_of(album.artists), count,
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
  s.dto = artistItem(artist, artist_id, count, artist.artwork ? "1" : "");
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
      ids.emplace_back(makeId(kKindTrack, song.track_id));
      ++matched;
      totalMs += song.duration_ms;
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

  int pk = 0;
  int pid = 0;
  const bool hasParent = parseId(parent, pk, pid);

  const media_library::CatalogSnapshot snap = library().snapshot();

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
  if (!explicitMode && hasParent) {
    if (pk == kKindAlbum || pk == kKindGenre) {
      mode = Mode::Tracks;
    } else if (pk == kKindArtist) {
      mode = Mode::Albums;
    }
  }

  const auto internal_of = [](const media_library::SourceRef &ref) {
    const auto value = library().internal_id_of(ref);
    return value ? *value : 0;
  };
  const auto has_artist = [&](const std::vector<media_library::ArtistLink> &links,
                              int id) {
    for (const auto &link : links) {
      if (internal_of(link.ref) == id) {
        return true;
      }
    }
    return false;
  };
  const auto has_album_artist =
      [&](const std::vector<media_library::AlbumArtistLink> &links, int id) {
        for (const auto &link : links) {
          if (internal_of(link.ref) == id) {
            return true;
          }
        }
        return false;
      };
  const auto parse_any = [](const std::vector<std::string> &values, int kind) {
    std::vector<int> out;
    for (const auto &text : values) {
      int k = 0;
      int v = 0;
      if (parseId(text, k, v) && k == kind) {
        out.push_back(v);
      }
    }
    return out;
  };
  const auto contains_id = [](const std::vector<int> &ids, int id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
  };

  std::vector<Sortable> items;
  if (mode == Mode::Tracks) {
    const std::vector<int> album_ids = parse_any(wantAlbumIds, kKindAlbum);
    const std::vector<int> artist_ids = parse_any(wantArtistIds, kKindArtist);
    const std::vector<int> contrib_ids = parse_any(wantContribArtists, kKindArtist);
    const std::vector<int> album_artist_ids =
        parse_any(wantAlbumArtistIds, kKindArtist);
    const std::vector<int> genre_ids = parse_any(wantGenreIds, kKindGenre);
    for (std::size_t i = 0; i < snap.tracks.size(); ++i) {
      const auto &t = snap.tracks[i];
      const int track_id = snap.track_internal_ids[i];
      if (hasParent && pk == kKindAlbum) {
        if (!t.album || internal_of(t.album->ref) != pid) {
          continue;
        }
      } else if (hasParent && pk == kKindArtist) {
        if (!has_artist(t.artists, pid)) {
          continue;
        }
      } else if (hasParent && pk == kKindGenre) {
        bool matched = false;
        const auto git = snap.track_genres.find(track_id);
        if (git != snap.track_genres.end()) {
          for (const auto &genre : git->second) {
            if (genre.internal_id == pid) {
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
        const int album_id = t.album ? internal_of(t.album->ref) : 0;
        if (!contains_id(album_ids, album_id)) {
          continue;
        }
      }
      if (!artist_ids.empty() || !contrib_ids.empty()) {
        const auto &list = artist_ids.empty() ? contrib_ids : artist_ids;
        bool ok = false;
        for (const int id : list) {
          if (has_artist(t.artists, id) || has_album_artist(t.album_artists, id)) {
            ok = true;
            break;
          }
        }
        if (!ok) {
          continue;
        }
      }
      if (!album_artist_ids.empty()) {
        bool ok = false;
        for (const int id : album_artist_ids) {
          if (has_album_artist(t.album_artists, id)) {
            ok = true;
            break;
          }
        }
        if (!ok) {
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
    const std::vector<int> artist_ids = parse_any(wantArtistIds, kKindArtist);
    const std::vector<int> album_artist_ids =
        parse_any(wantAlbumArtistIds, kKindArtist);
    for (std::size_t i = 0; i < snap.albums.size(); ++i) {
      const auto &a = snap.albums[i];
      if (hasParent && pk == kKindArtist) {
        if (!has_album_artist(a.artists, pid)) {
          continue;
        }
      }
      if (!search.empty() && lower(a.title).find(search) == std::string::npos) {
        continue;
      }
      if (!artist_ids.empty() || !album_artist_ids.empty()) {
        const auto &list =
            artist_ids.empty() ? album_artist_ids : artist_ids;
        bool ok = false;
        for (const int id : list) {
          if (has_album_artist(a.artists, id)) {
            ok = true;
            break;
          }
        }
        if (!ok) {
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
  const media_library::CatalogSnapshot snap = library().snapshot();
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
  int kind = 0;
  int id = 0;
  if (!parseId(idstr, kind, id)) {
    return http_util::jsonResponse(404, "{}");
  }
  crow::json::wvalue dto;
  bool found = false;
  if (kind == kKindTrack) {
    const auto ref =
        library().ref_for_compat(media_library::EntityType::Track, id);
    if (ref) {
      const auto result = library().get_track(*ref, {});
      if (result.ok()) {
        const auto &t = result.value();
        const int albumId = t.album ? compat_id_of(t.album->ref) : 0;
        const auto source = library().resolve_playback(*ref, {});
        const std::string filePath =
            source.ok() ? source.value().internal_locator : std::string();
        std::vector<std::pair<std::string, int>> genres;
        if (!t.genres.empty()) {
          const media_library::CatalogSnapshot snap = library().snapshot();
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
        dto = trackItem(t, id, albumId,
                        t.album ? t.album->title : std::string(), filePath,
                        artist_pairs_of(t.artists),
                        album_artist_pairs_of(t.album_artists), genres);
        found = true;
      }
    }
  } else if (kind == kKindAlbum) {
    const auto ref =
        library().ref_for_compat(media_library::EntityType::Album, id);
    if (ref) {
      const auto result = library().get_album(*ref, {});
      if (result.ok()) {
        const auto &a = result.value();
        int count = 0;
        long long runtimeMs = 0;
        const auto tracks =
            library().list_album_tracks(*ref, {std::nullopt, 1000000}, {});
        if (tracks.ok()) {
          count = static_cast<int>(tracks.value().items.size());
          for (const auto &track : tracks.value().items) {
            runtimeMs += track.duration_ms.value_or(0);
          }
        }
        dto = albumItem(a, id, album_artist_pairs_of(a.artists), count,
                        a.artwork ? "1" : "", runtimeMs * 10000LL);
        found = true;
      }
    }
  } else if (kind == kKindArtist) {
    const auto ref =
        library().ref_for_compat(media_library::EntityType::Artist, id);
    if (ref) {
      const auto result = library().get_artist(*ref, {});
      if (result.ok()) {
        const auto &a = result.value();
        int count = 0;
        const auto albums =
            library().list_artist_albums(*ref, {std::nullopt, 1000000}, {});
        if (albums.ok()) {
          count = static_cast<int>(albums.value().items.size());
        }
        dto = artistItem(a, id, count, a.artwork ? "1" : "");
        found = true;
      }
    }
  } else if (kind == kKindGenre) {
    const media_library::CatalogSnapshot snap = library().snapshot();
    for (const auto &g : snap.genres) {
      if (g.internal_id == id) {
        dto = genreItem(id, g.name);
        found = true;
        break;
      }
    }
  } else if (kind == kKindView) {
    dto = musicView();
    found = true;
  }
  if (!found) {
    return http_util::jsonResponse(404, "{}");
  }
  return json(200, dto);
}

crow::response handleImage(const std::string &idstr) {
  // 歌单封面：每次请求随机返回歌单内一首已匹配曲目的专辑封面。
  playlist::Playlist p;
  if (playlist::get(idstr, p)) {
    std::vector<std::string> covers;
    for (const auto &s : p.songs) {
      if (!s.matched) {
        continue;
      }
      std::string cover;
      const auto trackRef =
          library().ref_for_compat(media_library::EntityType::Track,
                                   s.track_id);
      if (trackRef) {
        const auto artwork = library().artwork_path(*trackRef);
        if (artwork) {
          cover = *artwork;
        }
      }
      if (!cover.empty()) {
        covers.push_back(std::move(cover));
      }
    }
    if (covers.empty()) {
      return http_util::jsonResponse(404, "{}");
    }
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<std::size_t> dist(0, covers.size() - 1);
    return http_util::serveImage(covers[dist(rng)]);
  }
  int kind = 0;
  int id = 0;
  if (!parseId(idstr, kind, id)) {
    return http_util::jsonResponse(404, "{}");
  }
  std::optional<media_library::EntityType> entityType;
  if (kind == kKindTrack) {
    entityType = media_library::EntityType::Track;
  } else if (kind == kKindAlbum) {
    entityType = media_library::EntityType::Album;
  } else if (kind == kKindArtist) {
    entityType = media_library::EntityType::Artist;
  }
  if (!entityType) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto ref = library().ref_for_compat(*entityType, id);
  if (!ref) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto artwork = library().artwork_path(*ref);
  if (!artwork) {
    return http_util::jsonResponse(404, "{}");
  }
  return http_util::serveImage(*artwork);
}

crow::response handleStream(const crow::request &req,
                            const std::string &idstr) {
  int kind = 0;
  int id = 0;
  if (!parseId(idstr, kind, id) || kind != kKindTrack) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto ref = library().ref_for_compat(media_library::EntityType::Track, id);
  if (!ref) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto source = library().resolve_playback(*ref, {});
  if (!source.ok()) {
    return http_util::jsonResponse(404, "{}");
  }
  return http_util::serveResource(source.value().internal_locator,
                                  req.get_header_value("Range"));
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
  int kind = 0;
  int id = 0;
  if (!parseId(idstr, kind, id) || kind != kKindTrack) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto ref = library().ref_for_compat(media_library::EntityType::Track, id);
  if (!ref) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto lyricsResult = library().get_lyrics(*ref, {});
  const auto trackResult = library().get_track(*ref, {});
  if (!lyricsResult.ok() || !trackResult.ok()) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto &lyrics = lyricsResult.value();
  const std::string plain = lyrics.plain_text.value_or(std::string());
  if (plain.empty()) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto &track = trackResult.value();
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
  const media_library::CatalogSnapshot snap = library().snapshot();
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
  const media_library::CatalogSnapshot snap = library().snapshot();
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
  const media_library::CatalogSnapshot snap = library().snapshot();
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
  int kind = 0, id = 0;
  if (!parseId(idstr, kind, id) || kind != kKindTrack) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto ref = library().ref_for_compat(media_library::EntityType::Track, id);
  if (!ref) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto result = library().get_track(*ref, {});
  const auto source = library().resolve_playback(*ref, {});
  if (!result.ok() || !source.ok()) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto &t = result.value();
  const long long durationMs = t.duration_ms.value_or(0);
  const int bitrateKbps = t.audio.bitrate_kbps.value_or(0);
  const std::string url =
      "/Audio/" + idstr + "/stream?static=true&mediaSourceId=" + idstr;
  const long long bitrate_bps = static_cast<long long>(bitrateKbps) * 1000LL;
  crow::json::wvalue src;
  src["Id"] = idstr;
  src["Name"] = t.title;
  src["Path"] = source.value().internal_locator;
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
crow::response handleImageType(const std::string &idstr, const std::string &imageType,
                               const std::string &) {
  if (lower(imageType) != "primary") {
    return http_util::jsonResponse(404, "{}");
  }
  return handleImage(idstr);
}

// 相似条目：同专辑 + 同流派的其它曲目。
crow::response handleSimilar(const std::string &idstr) {
  std::vector<Sortable> items;
  int kind = 0, id = 0;
  if (parseId(idstr, kind, id) && kind == kKindTrack) {
    const media_library::CatalogSnapshot snap = library().snapshot();
    std::size_t origin = snap.tracks.size();
    for (std::size_t i = 0; i < snap.track_internal_ids.size(); ++i) {
      if (snap.track_internal_ids[i] == id) {
        origin = i;
        break;
      }
    }
    if (origin < snap.tracks.size()) {
      const int album_id = snap.tracks[origin].album
                               ? compat_id_of(snap.tracks[origin].album->ref)
                               : 0;
      std::unordered_set<int> want_genres;
      const auto genres = snap.track_genres.find(id);
      if (genres != snap.track_genres.end()) {
        for (const auto &g : genres->second) {
          want_genres.insert(g.internal_id);
        }
      }
      std::unordered_set<int> seen{id};
      constexpr std::size_t kLimit = 20;
      auto add_track = [&](std::size_t index) {
        if (items.size() >= kLimit) {
          return;
        }
        const int track_id = snap.track_internal_ids[index];
        if (!seen.insert(track_id).second) {
          return;
        }
        items.push_back(itemForTrack(snap, index));
      };
      if (album_id > 0) {
        for (std::size_t i = 0; i < snap.tracks.size() && items.size() < kLimit;
             ++i) {
          const auto &other = snap.tracks[i];
          if (other.album && compat_id_of(other.album->ref) == album_id) {
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
  int kind = 0, id = 0;
  if (!parseId(idstr, kind, id) || kind != kKindTrack) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto ref = library().ref_for_compat(media_library::EntityType::Track, id);
  if (!ref) {
    return http_util::jsonResponse(404, "{}");
  }
  const auto source = library().resolve_playback(*ref, {});
  if (!source.ok()) {
    return http_util::jsonResponse(404, "{}");
  }
  return http_util::serveResource(source.value().internal_locator,
                                  req.get_header_value("Range"));
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
    const media_library::CatalogSnapshot snap = library().snapshot();
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
      const int track_id = snap.track_internal_ids[i];
      push(makeId(kKindTrack, track_id),
           t.title.empty() ? ("Track " + std::to_string(track_id)) : t.title,
           "Audio");
    }
    for (std::size_t i = 0; i < snap.albums.size() && (int)hints.size() < cap;
         ++i) {
      const auto &a = snap.albums[i];
      if (lower(a.title).find(lterm) != std::string::npos) {
        push(makeId(kKindAlbum, snap.album_internal_ids[i]), a.title,
             "MusicAlbum");
      }
    }
    for (std::size_t i = 0; i < snap.artists.size() && (int)hints.size() < cap;
         ++i) {
      const auto &a = snap.artists[i];
      if (lower(a.name).find(lterm) != std::string::npos) {
        push(makeId(kKindArtist, snap.artist_internal_ids[i]), a.name,
             "MusicArtist");
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
  const media_library::CatalogSnapshot snap = library().snapshot();
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

// 按名字（或 GUID）取单个艺术家。
crow::response handleArtistByName(const std::string &raw) {
  int kind = 0, id = 0;
  const media_library::CatalogSnapshot snap = library().snapshot();
  std::optional<std::size_t> index;
  if (parseId(raw, kind, id) && kind == kKindArtist) {
    for (std::size_t i = 0; i < snap.artist_internal_ids.size(); ++i) {
      if (snap.artist_internal_ids[i] == id) {
        index = i;
        break;
      }
    }
  } else {
    const std::string name = http_util::urlDecode(raw);
    for (std::size_t i = 0; i < snap.artists.size(); ++i) {
      if (snap.artists[i].name == name) {
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
  int kind = 0, id = 0;
  const media_library::CatalogSnapshot snap = library().snapshot();
  int artist_id = -1;
  if (parseId(raw, kind, id) && kind == kKindArtist) {
    artist_id = id;
  } else {
    const std::string name = http_util::urlDecode(raw);
    for (std::size_t i = 0; i < snap.artists.size(); ++i) {
      if (snap.artists[i].name == name) {
        artist_id = snap.artist_internal_ids[i];
        break;
      }
    }
  }
  if (artist_id < 0) {
    return http_util::jsonResponse(404, "{}");
  }

  std::unordered_set<int> genre_ids;
  for (std::size_t i = 0; i < snap.tracks.size(); ++i) {
    const int track_id = snap.track_internal_ids[i];
    bool by_artist = false;
    for (const auto &link : snap.tracks[i].artists) {
      if (compat_id_of(link.ref) == artist_id) {
        by_artist = true;
        break;
      }
    }
    if (!by_artist) {
      continue;
    }
    const auto genres = snap.track_genres.find(track_id);
    if (genres == snap.track_genres.end()) {
      continue;
    }
    for (const auto &g : genres->second) {
      genre_ids.insert(g.internal_id);
    }
  }

  std::vector<int> artist_ids;
  std::unordered_set<int> seen{artist_id};
  constexpr std::size_t kLimit = 20;
  for (std::size_t i = 0; i < snap.tracks.size() && artist_ids.size() < kLimit;
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
      const int aid = compat_id_of(link.ref);
      if (aid <= 0 || !seen.insert(aid).second) {
        continue;
      }
      artist_ids.push_back(aid);
      if (artist_ids.size() >= kLimit) {
        break;
      }
    }
  }

  std::vector<Sortable> items;
  for (const int aid : artist_ids) {
    for (std::size_t i = 0; i < snap.artist_internal_ids.size(); ++i) {
      if (snap.artist_internal_ids[i] == aid) {
        items.push_back(itemForArtist(snap, i));
        break;
      }
    }
  }
  return respondItems(items, "", "Ascending", 0, 0);
}

// 解析逗号分隔的 track GUID 列表为 track id 列表。
std::vector<int> parseTrackIds(const std::string &csv) {
  std::vector<int> ids;
  for (const auto &tok : splitCsv(csv)) {
    int k = 0, v = 0;
    if (parseId(tok, k, v) && k == kKindTrack) {
      ids.push_back(v);
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

// 从 JSON body 的某个 GUID 数组字段追加 track id。
void appendBodyIds(const crow::json::rvalue &body, const char *name,
                   std::vector<int> &out) {
  if (!body || !body.has(name) ||
      body[name].t() != crow::json::type::List) {
    return;
  }
  for (const auto &x : body[name]) {
    int k = 0, v = 0;
    if (x.t() == crow::json::type::String &&
        parseId(std::string(x.s()), k, v) && k == kKindTrack) {
      out.push_back(v);
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
  const media_library::CatalogSnapshot snap = library().snapshot();
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
    item.dto["PlaylistItemId"] = makeId(kKindTrack, s.track_id);
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
  ([](const crow::request &, std::string itemId, std::string imageType, std::string imageIndex) {
    return handleImageType(itemId, imageType, imageIndex);
  });
  JF_ROUTE(app, "/Items/<string>/Images/<string>")
  ([](const crow::request &, std::string itemId, std::string imageType) {
    return handleImageType(itemId, imageType, "");
  });
  JF_ROUTE(app, "/Items/<string>/Download")
  ([](const crow::request &req, std::string itemId) { return handleDownload(req, itemId); });
  JF_ROUTE(app, "/Items/<string>/File")
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
  ([](const crow::request &, std::string name, std::string imageType) {
    return handleImageType(name, imageType, "");
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
