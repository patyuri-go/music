#include "jellyfin.h"

#include "config.h"
#include "dto.h"
#include "http_util.h"
#include "ids.h"
#include "media_library/media_runtime.h"
#include "stream_http.h"

#include "crow/app.h"
#include "crow/http_request.h"
#include "crow/http_response.h"
#include "crow/json.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace jellyfin {
namespace {

namespace fs = std::filesystem;

constexpr const char *kVersion = "10.10.7";

// 门面只经 MediaRouter / stream_http 取数据：不持有本地库对象、不读本地快照、
// 不遍历库。「不能靠路由器实现的端点」按约定返回空，而不是补本地特例。

// 目录实体的对外 id 就是 SourceRef 的规范字符串（见 ids.h 的 itemId）。
// 它不依赖数据库自增主键：重扫后不变，远端 provider 也能给出合法 id。
using IdPair = std::pair<std::string, std::string>;

std::vector<IdPair>
artist_pairs_of(const std::vector<media_library::ArtistLink> &links) {
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
    if (apply_methods)
      apply_methods(r);
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
    const std::string token = hay.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos);
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

// -------------------------------------------------------------------- 分页

// 门面一律不排序：顺序完全由 media_library 决定（聚合结果按配置的
// library_priority 分层，单库结果用该库自己的自然顺序）。客户端传来的
// sortBy / sortOrder 被忽略——门面重排会打散聚合 tier，且远端库没有
// 全量数据，无法给出「全局有序」的保证。
crow::response respondItems(std::vector<crow::json::wvalue> &items,
                            int startIndex, int limit) {
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
    out.push_back(std::move(items[i]));
  }
  return json(200, queryResult(std::move(out), total, from));
}

// ------------------------------------------------- 快照到 Jellyfin DTO 的映射

// 快照只携带统一领域模型；兼容层负责把 SourceRef 映射成对外 ID。
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
  r["CompletedInstallations"] = crow::json::wvalue(crow::json::wvalue::list{});
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
    std::string part = (comma == std::string::npos)
                           ? s.substr(start)
                           : s.substr(start, comma - start);
    if (!part.empty())
      out.push_back(part);
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return out;
}

crow::json::wvalue remoteTrackItem(const media_library::Track &track) {
  const std::string album_key =
      track.album.has_value() ? track.album->title : std::string();
  std::vector<std::pair<std::string, int>> genres;
  genres.reserve(track.genres.size());
  for (const auto &name : track.genres) {
    genres.emplace_back(name, 0);
  }
  // Path 是 Jellyfin 的文件系统字段，远端没有；locator 也不出进程。
  return trackItem(track, itemId(track.ref),
                   track.album.has_value() ? itemId(track.album->ref)
                                           : std::string(),
                   album_key, std::string(), artist_pairs_of(track.artists),
                   album_artist_pairs_of(track.album_artists), genres);
}

crow::json::wvalue remoteAlbumItem(const media_library::Album &album) {
  return albumItem(album, itemId(album.ref),
                   album_artist_pairs_of(album.artists), 0,
                   album.artwork.has_value() ? "1" : "", 0);
}

// 搜索命中 → 列表条目。SearchHit 只带描述字段，够造列表 DTO。
crow::json::wvalue searchHitItem(const media_library::SearchHit &hit) {
  const std::string id = itemId(hit.ref);
  if (hit.kind == media_library::EntityType::Track) {
    media_library::Track track;
    track.ref = hit.ref;
    track.title = hit.title;
    track.artists = hit.artists;
    track.album_artists = hit.album_artists;
    track.artwork = hit.artwork;
    track.duration_ms = hit.duration_ms;
    track.playable = hit.playable;
    track.album = hit.album;
    // 命中要能连回可打开的专辑页：没有 AlbumId/ParentId 的 Audio 条目
    // 在客户端里点进去只会发 ParentId=<曲目 ref> 的请求，必然是空列表。
    return trackItem(track, id,
                     hit.album.has_value() ? itemId(hit.album->ref)
                                           : std::string(),
                     hit.album.has_value() ? hit.album->title : std::string(),
                     std::string(), artist_pairs_of(track.artists),
                     album_artist_pairs_of(track.album_artists), {});
  }
  if (hit.kind == media_library::EntityType::Album) {
    media_library::Album album;
    album.ref = hit.ref;
    album.title = hit.title;
    album.artists = hit.album_artists;
    album.artwork = hit.artwork;
    return albumItem(album, id, album_artist_pairs_of(album.artists), 0,
                     album.artwork.has_value() ? "1" : "", 0);
  }
  media_library::Artist artist;
  artist.ref = hit.ref;
  artist.name = hit.title;
  artist.artwork = hit.artwork;
  return artistItem(artist, id, 0, artist.artwork.has_value() ? "1" : "");
}

// 列表查询只经 MediaRouter：
//   searchTerm            → 聚合搜索（本地 + 远端，由路由器内部归并）
//   parentId = 专辑        → list_album_tracks
//   parentId = 艺术家      → list_artist_albums
//   ArtistIds/AlbumArtistIds 指定艺术家 → list_artist_albums
// 其余（Music 视图根等）没有路由器能力，返回空列表。
crow::response handleItems(const crow::request &req) {
  const std::string parent = qs(req, "parentId");
  const std::string include = qs(req, "includeItemTypes");
  const std::string search = qs(req, "searchTerm");
  const int startIndex = qsInt(req, "startIndex", 0);
  const int limit = qsInt(req, "limit", 0);
  // sortBy / sortOrder 一律不读：门面不重排，顺序由 media_library 给出。

  const std::optional<media_library::SourceRef> parentRef =
      parent.empty() ? std::nullopt : parseItemId(parent);

  const auto parse_ids = [](const std::vector<std::string> &values,
                            media_library::EntityType type) {
    std::vector<media_library::SourceRef> out;
    for (const auto &text : values) {
      const auto ref = parseItemId(text);
      if (ref.has_value() && ref->entity_type == type) {
        out.push_back(*ref);
      }
    }
    return out;
  };

  std::vector<crow::json::wvalue> items;

  if (!search.empty()) {
    media_library::SearchQuery query;
    query.text = search;
    if (listHas(include, "MusicAlbum")) {
      query.types.push_back(media_library::EntityType::Album);
    }
    if (listHas(include, "MusicArtist")) {
      query.types.push_back(media_library::EntityType::Artist);
    }
    if (listHas(include, "Audio") || listHas(include, "MusicVideo") ||
        query.types.empty()) {
      query.types.push_back(media_library::EntityType::Track);
    }
    query.page = media_library::PageRequest{
        std::nullopt, limit > 0 ? std::max(limit, 200) : 200};
    // 客户端要的是哪几种实体就只回哪几种：库没按 types 过滤时也不能让
    // 别的类型混进来（「专辑」页里出现 Audio 条目就是点不开的那种）。
    const auto wanted = [&query](media_library::EntityType kind) {
      return std::find(query.types.begin(), query.types.end(), kind) !=
             query.types.end();
    };
    const bool all_kinds = query.types.empty();
    const auto page = media_library::media_router().search(query, {});
    if (page.ok()) {
      for (const auto &sourced : page.value().items) {
        const media_library::SearchHit &hit = sourced.value;
        const bool keep =
            all_kinds || wanted(hit.kind) ||
            (hit.kind == media_library::EntityType::AlbumArtist &&
             wanted(media_library::EntityType::Artist));
        if (!keep) {
          continue;
        }
        items.push_back(searchHitItem(hit));
      }
    }
    return respondItems(items, startIndex, limit);
  }

  if (parentRef.has_value() &&
      parentRef->entity_type == media_library::EntityType::Album) {
    const auto page = media_library::media_router().list_album_tracks(
        *parentRef, {std::nullopt, 1000000}, {});
    if (page.ok()) {
      for (const auto &track : page.value().value.items) {
        items.push_back(remoteTrackItem(track));
      }
    }
    return respondItems(items, startIndex, limit);
  }

  std::vector<media_library::SourceRef> artist_refs;
  if (parentRef.has_value() &&
      (parentRef->entity_type == media_library::EntityType::Artist ||
       parentRef->entity_type == media_library::EntityType::AlbumArtist)) {
    artist_refs.push_back(*parentRef);
  }
  for (const auto &ref :
       parse_ids(splitCsv(qs(req, "artistIds")),
                 media_library::EntityType::Artist)) {
    artist_refs.push_back(ref);
  }
  for (const auto &ref :
       parse_ids(splitCsv(qs(req, "artistIds")),
                 media_library::EntityType::AlbumArtist)) {
    artist_refs.push_back(ref);
  }
  for (const auto &ref :
       parse_ids(splitCsv(qs(req, "albumArtistIds")),
                 media_library::EntityType::AlbumArtist)) {
    artist_refs.push_back(ref);
  }

  if (!artist_refs.empty()) {
    std::unordered_set<std::string> seen;
    for (const auto &raw : artist_refs) {
      media_library::SourceRef copy = raw;
      const media_library::ArtistRef artist_ref =
          media_library::as_artist_ref(copy);
      const auto page = media_library::media_router().list_artist_albums(
          artist_ref, {std::nullopt, 1000000}, {});
      if (!page.ok()) {
        continue;
      }
      for (const auto &album : page.value().value.items) {
        if (!seen.insert(itemId(album.ref)).second) {
          continue;
        }
        items.push_back(remoteAlbumItem(album));
      }
    }
    return respondItems(items, startIndex, limit);
  }

  // Music 视图根等：路由器没有列举能力，远程媒体库又不允许全量扫描。
  return emptyList();
}

crow::response handleLatest(const crow::request &req) {
  // 「最近添加」需要按 added_at 跨库排序，路由器没有该能力，不做本地特例。
  (void)req;
  return emptyList();
}

crow::response handleItem(const std::string &idstr) {
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
        // Path 是 Jellyfin 的文件系统字段；locator
        // 不出进程，这里留空（5.4.4）。
        const std::string filePath;
        // 流派 id 是本地库内部概念，路由器只给名字；恒填 0 与远端条目一致。
        std::vector<std::pair<std::string, int>> genres;
        genres.reserve(t.genres.size());
        for (const auto &name : t.genres) {
          genres.emplace_back(name, 0);
        }
        return json(200,
                    trackItem(t, itemId(t.ref),
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
        const auto tracks = media_library::media_router().list_album_tracks(
            *ref, {std::nullopt, 1000000}, {});
        if (tracks.ok()) {
          count = static_cast<int>(tracks.value().value.items.size());
          for (const auto &track : tracks.value().value.items) {
            runtimeMs += track.duration_ms.value_or(0);
          }
        }
        return json(200, albumItem(a, itemId(a.ref),
                                   album_artist_pairs_of(a.artists), count,
                                   a.artwork ? "1" : "", runtimeMs * 10000LL));
      }
    } else if (ref->entity_type == media_library::EntityType::Artist ||
               ref->entity_type == media_library::EntityType::AlbumArtist) {
      const media_library::ArtistRef artist_ref =
          ref->entity_type == media_library::EntityType::AlbumArtist
              ? media_library::as_artist_ref(*ref)
              : *ref;
      const auto result =
          media_library::media_router().get_artist(artist_ref, {});
      if (result.ok()) {
        const auto &a = result.value().value;
        int count = 0;
        const auto albums = media_library::media_router().list_artist_albums(
            artist_ref, {std::nullopt, 1000000}, {});
        if (albums.ok()) {
          count = static_cast<int>(albums.value().value.items.size());
        }
        return json(200,
                    artistItem(a, itemId(a.ref), count, a.artwork ? "1" : ""));
      }
    }
  }

  // 流派不是路由器实体（没有 SourceRef，int id 也无远端对应），不做特例实现。
  return http_util::jsonResponse(404, "{}");
}

// 封面字节统一从 ArtworkService 出：选源交给 MediaRouter，locator 不外泄。
crow::response handleImage(const crow::request &req, const std::string &idstr) {
  const bool head_only = req.method == crow::HTTPMethod::HEAD;

  // 歌单没有独立的封面实体（封面属于专辑），路由器不提供该能力：歌单 id 走到
  // 下面的 parse_path_ref 后自然 404，不做本地特例实现。
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
  return stream_http::serve_track(req, *ref,
                                  req.method == crow::HTTPMethod::HEAD);
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
  const auto lyricsResult =
      media_library::media_router().resolve_lyrics(*ref, {});
  const auto trackResult =
      media_library::media_router().resolve_track(*ref, {});
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
  const std::string album = track.album ? track.album->title : std::string();
  return json(200,
              buildLyricDto(track.title, track.duration_ms.value_or(0), plain,
                            lyrics.mode != media_library::LyricsMode::Unsynced,
                            artist, album));
}

crow::response handleArtists(const crow::request &req,
                             bool album_only = false) {
  // 艺术家列表需要全库列举，路由器没有聚合 list_artists。
  (void)req;
  (void)album_only;
  return emptyList();
}

crow::response handleGenres(const crow::request &req) {
  // 路由器没有流派枚举能力：流派的 int id 是本地库内部概念，远端无对应。
  // 按「不能靠路由器实现的不要实现」直接返回空列表。
  return emptyList();
}

crow::response handleMusicGenreByName(const std::string &name) {
  // 同上：按名字取流派依赖本地流派的 int id，路由器不提供。
  (void)name;
  return http_util::jsonResponse(404, "{}");
}

// ------------------------------------------------------------ 只读扩展接口

// 播放信息：返回单个 MediaSource，供客户端直接播放原始文件。
crow::response handlePlaybackInfo(const crow::request &,
                                  const std::string &idstr) {
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
crow::response handleImageType(const crow::request &req,
                               const std::string &idstr,
                               const std::string &imageType,
                               const std::string &) {
  if (lower(imageType) != "primary") {
    return http_util::jsonResponse(404, "{}");
  }
  return handleImage(req, idstr);
}

// 相似条目：同专辑 + 同流派的其它曲目。
crow::response handleSimilar(const std::string &idstr) {
  // 路由器没有「相似曲目」能力，不做本地特例实现。
  (void)idstr;
  return emptyList();
}

// 下载原始文件（与 /Items/<id>/File 同义）。
crow::response handleDownload(const crow::request &req,
                              const std::string &idstr) {
  const auto ref = parse_path_ref(idstr);
  if (!ref.has_value() ||
      ref->entity_type != media_library::EntityType::Track) {
    return http_util::jsonResponse(404, "{}");
  }
  // 下载与在线播放走同一条会话路径：字节来源一致，Range 语义一致。
  return stream_http::serve_track(req, *ref,
                                  req.method == crow::HTTPMethod::HEAD);
}

// 搜索建议：曲目 / 专辑 / 艺术家。
crow::response handleSearchHints(const crow::request &req) {
  // typeahead 是前缀匹配，SearchQuery 只有包含匹配；语义不等价，不实现。
  (void)req;
  return emptyList();
}

// 过滤选项：真实流派与年份。
crow::response handleFilters(const crow::request &) {
  // 流派枚举无路由器能力；年份需要遍历全部曲目，同样没有聚合入口。
  // 保留响应形状，内容为空。
  crow::json::wvalue r;
  r["Genres"] = crow::json::wvalue(crow::json::wvalue::list{});
  r["Tags"] = crow::json::wvalue(crow::json::wvalue::list{});
  r["OfficialRatings"] = crow::json::wvalue(crow::json::wvalue::list{});
  r["Years"] = crow::json::wvalue(crow::json::wvalue::list{});
  return json(200, r);
}

// 按稳定 id（或名字）取单个艺术家。
crow::response handleArtistByName(const std::string &raw) {
  // 按名字精确取艺术家不是路由器能力（SearchQuery 只有包含匹配）。
  (void)raw;
  return http_util::jsonResponse(404, "{}");
}

// 相似艺术家：与本艺术家共享流派的其它艺术家。
crow::response handleArtistSimilar(const std::string &raw) {
  // 路由器没有「相似艺术家」能力。
  (void)raw;
  return emptyList();
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
  if (!body || !body.has(name) || body[name].t() != crow::json::type::List) {
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
  // 本地歌单是文件式子系统的私有概念，路由器只认 PlaylistRef。
  return emptyList();
}

// 歌单详情。
crow::response handlePlaylistById(const std::string &idstr) {
  (void)idstr;
  return http_util::jsonResponse(404, "{}");
}

// 歌单内的曲目。
crow::response handlePlaylistItems(const std::string &idstr) {
  (void)idstr;
  return emptyList();
}

// 新建歌单（name/ids 可来自 query 或 JSON body）。
crow::response handleCreatePlaylist(const crow::request &req) {
  // 歌单写入没有路由器能力（PlaylistWriter 不存在，PlaylistWrite
  // 能力从未注册）。
  (void)req;
  return http_util::jsonResponse(501, "{}");
}

// 向歌单添加曲目（ids 可来自 query 或 JSON body）。
crow::response handleAddToPlaylist(const crow::request &req,
                                   const std::string &idstr) {
  (void)req;
  (void)idstr;
  return http_util::jsonResponse(501, "{}");
}

// 从歌单移除曲目（entryIds / ids 均按曲目 GUID 处理，可来自 query 或 body）。
crow::response handleRemoveFromPlaylist(const crow::request &req,
                                        const std::string &idstr) {
  (void)req;
  (void)idstr;
  return http_util::jsonResponse(501, "{}");
}

// 更新歌单：改名与/或替换曲目（name/ids 可来自 query 或 body）。
crow::response handleUpdatePlaylist(const crow::request &req,
                                    const std::string &idstr) {
  (void)req;
  (void)idstr;
  return http_util::jsonResponse(501, "{}");
}

// 删除歌单。
crow::response handleDeletePlaylist(const std::string &idstr) {
  (void)idstr;
  return http_util::jsonResponse(501, "{}");
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
  JF_ROUTE(app, "/System/Ping")
      .methods(crow::HTTPMethod::Post)([](const crow::request &) {
        return crow::response(200, "Jellyfin Server");
      });
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
  JF_ROUTE(app, "/Users/AuthenticateByName")
      .methods(crow::HTTPMethod::Post)([](const crow::request &req) {
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
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post)(
          [](const crow::request &req, std::string itemId) {
            return handlePlaybackInfo(req, itemId);
          });
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
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Head)(
          [](const crow::request &req, std::string itemId) {
            return handleDownload(req, itemId);
          });
  JF_ROUTE(app, "/Items/<string>/File")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Head)(
          [](const crow::request &req, std::string itemId) {
            return handleDownload(req, itemId);
          });
  JF_ROUTE(app, "/Items/<string>/Similar")
  ([](const crow::request &, std::string itemId) {
    return handleSimilar(itemId);
  });
  JF_ROUTE(app, "/Items/<string>")
  ([](const crow::request &, std::string itemId) {
    return handleItem(itemId);
  });
  // 通用删除项（官方 DeleteItem）：目前仅支持删除歌单，其他类型返回 404。
  JF_ROUTE(app, "/Items/<string>")
      .methods(crow::HTTPMethod::Delete)(
          [](const crow::request &, std::string itemId) {
            return handleDeletePlaylist(itemId);
          });

  // 流媒体
  JF_ROUTE(app, "/Audio/<string>/Lyrics")
  ([](const crow::request &, std::string itemId) {
    return handleLyrics(itemId);
  });
  JF_ROUTE(app, "/Audio/<string>/<string>")
      .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Head)(
          [](const crow::request &req, std::string itemId, std::string) {
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
  ([](const crow::request &, std::string name) {
    return handleArtistSimilar(name);
  });
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
  JF_ROUTE(app, "/Sessions/Playing")
      .methods(crow::HTTPMethod::Post)(
          [](const crow::request &) { return crow::response(204); });
  JF_ROUTE(app, "/Sessions/Playing/Progress")
      .methods(crow::HTTPMethod::Post)(
          [](const crow::request &) { return crow::response(204); });
  JF_ROUTE(app, "/Sessions/Playing/Stopped")
      .methods(crow::HTTPMethod::Post)(
          [](const crow::request &) { return crow::response(204); });

  // 收藏
  JF_ROUTE(app, "/UserFavoriteItems/<string>")
      .methods(crow::HTTPMethod::Post)([](const crow::request &, std::string) {
        crow::json::wvalue r;
        r["IsFavorite"] = true;
        r["PlayCount"] = 0;
        r["Played"] = false;
        r["Key"] = "";
        return json(200, r);
      });
  JF_ROUTE(app, "/UserFavoriteItems/<string>")
      .methods(crow::HTTPMethod::Delete)(
          [](const crow::request &, std::string) {
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
  JF_ROUTE(app, "/DisplayPreferences/<string>")
      .methods(crow::HTTPMethod::Post)([](const crow::request &, std::string) {
        return crow::response(204);
      });
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
  JF_ROUTE(app, "/Playlists")
      .methods(crow::HTTPMethod::Post)(
          [](const crow::request &req) { return handleCreatePlaylist(req); });
  JF_ROUTE(app, "/Playlists/<string>")
  ([](const crow::request &, std::string id) {
    return handlePlaylistById(id);
  });
  JF_ROUTE(app, "/Playlists/<string>")
      .methods(crow::HTTPMethod::Post)(
          [](const crow::request &req, std::string id) {
            return handleUpdatePlaylist(req, id);
          });
  JF_ROUTE(app, "/Playlists/<string>")
      .methods(crow::HTTPMethod::Delete)(
          [](const crow::request &, std::string id) {
            return handleDeletePlaylist(id);
          });
  JF_ROUTE(app, "/Playlists/<string>/Items")
  ([](const crow::request &, std::string id) {
    return handlePlaylistItems(id);
  });
  JF_ROUTE(app, "/Playlists/<string>/Items")
      .methods(crow::HTTPMethod::Post)(
          [](const crow::request &req, std::string id) {
            return handleAddToPlaylist(req, id);
          });
  JF_ROUTE(app, "/Playlists/<string>/Items")
      .methods(crow::HTTPMethod::Delete)(
          [](const crow::request &req, std::string id) {
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
  app.port(cfg.jellyfin_port).multithreaded().concurrency(20).run();
}

} // namespace jellyfin
