#include "jellyfin.h"

#include "config.h"
#include "dto.h"
#include "http_util.h"
#include "ids.h"
#include "media_library/media_runtime.h"
#include "stream_http.h"

#include "http_server.h"
#include <json/json.h>

#include "json_util.h"

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

// Drogon 的路径参数保持原样（未解码），解析前先做百分号解码。
std::optional<media_library::SourceRef>
parse_path_ref(const std::string &text) {
  return parseItemId(http_util::urlDecode(text));
}

// 门面错误体：Jellyfin 用 ProblemDetails（不引第三方，按 OpenAPI 的
// components.schemas.ProblemDetails 手拼）。状态码仍是真实 HTTP 状态。
http_server::ResponsePtr problem_response(int status, const std::string &title,
                                         const std::string &detail = {}) {
  Json::Value problem;
  problem["type"] = "about:blank";
  problem["title"] = title;
  problem["status"] = status;
  if (!detail.empty()) {
    problem["detail"] = detail;
  }
  return http_util::jsonResponse(status, json_util::dump(problem));
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// 声明式路径（/Items/<string>/Images/<string>）→ Drogon 占位符。
std::string drogonPath(const std::string &raw_path) {
  std::string out;
  int index = 0;
  for (std::size_t i = 0; i < raw_path.size();) {
    if (raw_path[i] == '<') {
      const std::size_t close = raw_path.find('>', i);
      if (close == std::string::npos) {
        out.append(raw_path.substr(i));
        break;
      }
      ++index;
      out.push_back('{');
      out += std::to_string(index);
      out.push_back('}');
      i = close + 1;
      continue;
    }
    out.push_back(raw_path[i]);
    ++i;
  }
  return out;
}

// Jellyfin 路由声明助手：保留「路径 + .methods(...) + lambda」的写法，
// 内部翻译成 Drogon 注册。Drogon 的路径正则本身 icase，
// 所以不需要再额外注册小写形态（参数值也不会被改写）。
using AsyncCallback = std::function<void(const http_server::ResponsePtr &)>;

struct RouteBuilder {
  int port;
  std::string path;
  std::vector<drogon::HttpMethod> verbs{drogon::Get};

  RouteBuilder(int p, std::string raw) : port(p), path(drogonPath(raw)) {}

  template <typename... M> RouteBuilder &methods(M... m) {
    verbs = {m...};
    return *this;
  }

  template <typename F> void operator()(F f) const {
    for (const drogon::HttpMethod method : verbs) {
      http_server::onPort(port, method, path, f);
    }
  }

  // 异步 handler：void(req, callback, args...)。
  template <typename F> void async(F f) const {
    for (const drogon::HttpMethod method : verbs) {
      http_server::onPortAsync(port, method, path, f);
    }
  }
};

// 查询参数读取：Jellyfin 客户端使用 PascalCase（如 ParentId），
// 客户端大小写混用，这里统一做大小写不敏感匹配。
std::string qs(const http_server::RequestPtr &req, const char *key) {
  const std::optional<std::string> value =
      http_server::Query(req).get(std::string(key));
  return value.has_value() ? *value : std::string();
}

int qsInt(const http_server::RequestPtr &req, const char *key, int def) {
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

bool qsBool(const http_server::RequestPtr &req, const char *key, bool def) {
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

http_server::ResponsePtr json(int code, const Json::Value &v) {
  return http_util::jsonResponse(code, json_util::dump(v));
}

http_server::ResponsePtr emptyList() {
  Json::Value arr;
  arr = Json::Value(Json::arrayValue);
  return json(200, arr);
}

// -------------------------------------------------------------------- 分页

// 门面一律不排序：顺序完全由 media_library 决定（聚合结果按配置的
// library_priority 分层，单库结果用该库自己的自然顺序）。客户端传来的
// sortBy / sortOrder 被忽略——门面重排会打散聚合 tier，且远端库没有
// 全量数据，无法给出「全局有序」的保证。
http_server::ResponsePtr respondItems(std::vector<Json::Value> &items,
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
  std::vector<Json::Value> out;
  for (int i = from; i < to; ++i) {
    out.push_back(std::move(items[i]));
  }
  return json(200, queryResult(std::move(out), total, from));
}

// ------------------------------------------------- 快照到 Jellyfin DTO 的映射

// 快照只携带统一领域模型；兼容层负责把 SourceRef 映射成对外 ID。
// ---------------------------------------------------------------- 视图/系统

Json::Value musicView() {
  Json::Value v;
  v["Name"] = "Music";
  v["ServerId"] = serverId();
  v["Id"] = musicViewId();
  v["Type"] = "UserView";
  v["CollectionType"] = "music";
  v["IsFolder"] = true;
  return v;
}

Json::Value playlistsView() {
  Json::Value v;
  v["Name"] = "Playlists";
  v["ServerId"] = serverId();
  v["Id"] = playlistsViewId();
  v["Type"] = "UserView";
  v["CollectionType"] = "playlists";
  v["IsFolder"] = true;
  return v;
}

http_server::ResponsePtr userViews() {
  std::vector<Json::Value> items;
  items.push_back(musicView());
  items.push_back(playlistsView());
  return json(200, queryResult(std::move(items), 2, 0));
}

Json::Value publicSystemInfo() {
  Json::Value r;
  r["LocalAddress"] = "127.0.0.1";
  r["ServerName"] = serverName();
  r["Version"] = kVersion;
  r["ProductName"] = "Jellyfin Server";
  r["OperatingSystem"] = "Linux";
  r["Id"] = serverId();
  r["StartupWizardCompleted"] = true;
  return r;
}

Json::Value systemInfo() {
  Json::Value r = publicSystemInfo();
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
  r["CompletedInstallations"] = Json::Value(Json::Value(Json::arrayValue));
  r["CastReceiverApplications"] =
      Json::Value(Json::Value(Json::arrayValue));
  return r;
}

Json::Value brandingOptions() {
  Json::Value r;
  r["LoginDisclaimer"] = "";
  r["CustomCss"] = "";
  r["SplashscreenEnabled"] = false;
  return r;
}

Json::Value displayPreferences(const std::string &id) {
  Json::Value r;
  r["Id"] = id;
  r["ViewType"] = "";
  r["SortBy"] = "";
  r["IndexBy"] = "";
  r["RememberIndexing"] = false;
  r["PrimaryImageHeight"] = 250;
  r["PrimaryImageWidth"] = 250;
  Json::Value prefs;
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

Json::Value remoteTrackItem(const media_library::Track &track) {
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

Json::Value remoteAlbumItem(const media_library::Album &album) {
  return albumItem(album, itemId(album.ref),
                   album_artist_pairs_of(album.artists), 0,
                   album.artwork.has_value() ? "1" : "", 0);
}

// 搜索命中 → 列表条目。SearchHit 只带描述字段，够造列表 DTO。
Json::Value searchHitItem(const media_library::SearchHit &hit) {
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
// 歌单列表：只经播放列表总管（跨库汇总）。门面不排序、不遍历库、不换源。
// Jellyfin 客户端用 startIndex/limit 翻页，总管用不透明游标，因此每次向总管
// 取「startIndex + limit」条再切片——门面不持有游标，也不做缓存。
http_server::ResponsePtr playlistListPage(int startIndex, int limit) {
  const std::shared_ptr<media_library::PlaylistDirectory> directory =
      media_library::playlist_directory_ptr();
  if (directory == nullptr) {
    return emptyList();
  }
  const int from = startIndex < 0 ? 0 : startIndex;
  const int page_size = limit > 0 ? limit : 50;

  const auto result =
      directory->list_playlists({std::nullopt, from + page_size}, {});
  if (!result.ok()) {
    return emptyList();
  }

  std::vector<Json::Value> items;
  int index = 0;
  for (const auto &sourced : result.value().items) {
    if (index++ < from) {
      continue;
    }
    if (static_cast<int>(items.size()) >= page_size) {
      break;
    }
    // 列表卡片要曲目数与时长：总管列表只给摘要，这里逐个歌单展开一次。
    // 代价是每个歌单一次远端请求，按当前口径接受。
    int childCount = 0;
    long long runtimeTicks = 0;
    const auto full = directory->get_playlist(sourced.value.ref, {});
    if (full.ok()) {
      childCount = static_cast<int>(full.value().value.tracks.size());
      for (const auto &track : full.value().value.tracks) {
        runtimeTicks += track.duration_ms.value_or(0) * 10000LL;
      }
    }
    items.push_back(playlistItem(sourced.value, itemId(sourced.value.ref),
                                 childCount, runtimeTicks));
  }
  // 总管不提供 total，只能报「本次已知的条数」（契约缺口，见文档）。
  return json(200, queryResult(std::move(items),
                               from + static_cast<int>(items.size()), from));
}

http_server::ResponsePtr handleItems(const http_server::RequestPtr &req) {
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

  // 歌单视图根，或显式要 Playlist 类型：只经播放列表总管。
  if ((!parent.empty() && parent == playlistsViewId()) ||
      listHas(include, "Playlist")) {
    return playlistListPage(startIndex, limit);
  }

  std::vector<Json::Value> items;

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

http_server::ResponsePtr handleLatest(const http_server::RequestPtr &req) {
  // 「最近添加」需要按 added_at 跨库排序，路由器没有该能力，不做本地特例。
  (void)req;
  return emptyList();
}

http_server::ResponsePtr handleItem(const std::string &idstr) {
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
    } else if (ref->entity_type == media_library::EntityType::Playlist) {
      // 歌单详情：客户端列完歌单会逐条回来拉 Fields，只经播放列表总管。
      // ChildCount / RunTimeTicks 要曲目数，跟专辑一样顺手展开一次。
      const auto directory = media_library::playlist_directory_ptr();
      if (directory != nullptr) {
        const auto result = directory->get_playlist(*ref, {});
        if (result.ok()) {
          return json(200, playlistDto(result.value().value, itemId(*ref)));
        }
      }
    }
  }

  // 流派不是路由器实体（没有 SourceRef，int id 也无远端对应），不做特例实现。
  return problem_response(404, "not_found");
}

// 封面字节统一从 ArtworkService 出：选源交给 MediaRouter，locator 不外泄。
void handleImage(const http_server::RequestPtr &req, const std::string &idstr,
                 AsyncCallback callback) {
  const bool head_only = req->isHead();

  // 歌单没有独立的封面实体（封面属于专辑），路由器不提供该能力：歌单 id 走到
  // 下面的 parse_path_ref 后自然 404，不做本地特例实现。
  const auto ref = parse_path_ref(idstr);
  if (!ref.has_value() ||
      (ref->entity_type != media_library::EntityType::Track &&
       ref->entity_type != media_library::EntityType::Album &&
       ref->entity_type != media_library::EntityType::Artist &&
       ref->entity_type != media_library::EntityType::AlbumArtist)) {
    callback(problem_response(404, "not_found"));
    return;
  }
  media_library::ArtworkRef artwork;
  artwork.owner = ref->entity_type == media_library::EntityType::AlbumArtist
                      ? media_library::as_artist_ref(*ref)
                      : *ref;
  stream_http::serve_artwork_async(req, artwork, head_only,
                                   stream_http::Protocol::Jellyfin,
                                   std::move(callback));
}

void handleStream(const http_server::RequestPtr &req, const std::string &idstr,
                  AsyncCallback callback) {
  const auto ref = parse_path_ref(idstr);
  if (!ref.has_value() ||
      ref->entity_type != media_library::EntityType::Track) {
    callback(problem_response(404, "not_found"));
    return;
  }
  // 音频字节：本地库按实体 id 直连存储服务，远端库沿用同步路径。
  stream_http::serve_track_async(req, *ref, req->isHead(),
                                 stream_http::Protocol::Jellyfin,
                                 std::move(callback));
}

// 字符个数（按码点计）。Jellyfin 的 Cue.Position 是 Text 里的下标，
// .NET 用 UTF-16 code unit：CJK 与 ASCII 都是 1，代理对算 2。
std::size_t utf8_length(const std::string &text) {
  std::size_t count = 0;
  for (const char raw : text) {
    if ((static_cast<unsigned char>(raw) & 0xC0) != 0x80) {
      ++count;
    }
  }
  return count;
}

// 行内逐字轨 → Cue 数组。Position / EndPosition 是闭区间下标，
// Start / End 是 100ns tick（毫秒 × 10000）。
//
// 词文本必须正好拼成整行，否则下标无从对齐 —— 这种情况宁可不给逐字轨，
// 也不出半截 Cue 让客户端高亮错位。
Json::Value buildCues(const media_library::LyricLine &line) {
  Json::Value cues(Json::arrayValue);
  const std::vector<media_library::LyricWord> words =
      media_library::line_words(line);
  if (words.empty()) {
    return cues;
  }
  std::string joined;
  for (const media_library::LyricWord &word : words) {
    joined += word.text;
  }
  if (joined != media_library::line_text(line)) {
    return cues;
  }

  std::size_t pos = 0;
  for (std::size_t i = 0; i < words.size(); ++i) {
    const media_library::LyricWord &word = words[i];
    const std::size_t length = utf8_length(word.text);
    if (length == 0) {
      continue;
    }
    std::int64_t end_ms = word.start_ms;
    if (word.end_ms.has_value()) {
      end_ms = *word.end_ms;
    } else if (i + 1 < words.size()) {
      end_ms = words[i + 1].start_ms;
    } else if (line.end_ms.has_value()) {
      end_ms = *line.end_ms;
    }
    Json::Value cue;
    cue["Position"] = static_cast<int>(pos);
    cue["EndPosition"] = static_cast<int>(pos + length - 1);
    cue["Start"] = static_cast<Json::Int64>(word.start_ms * 10000LL);
    cue["End"] = static_cast<Json::Int64>(end_ms * 10000LL);
    cues.append(std::move(cue));
    pos += length;
  }
  return cues;
}

// 领域模型 → Jellyfin LyricDto。直接读结构，不再二次解析纯文本：
// 之前按纯文本解析，正文里带 `[..]` 会被当成时间标签吞掉（实测 78 行
// 的 Start 全丢）。逐字轨出 Cues；aux（译文/注音）不进 DTO。
Json::Value buildLyricDto(const media_library::Lyrics &lyrics,
                                 long long duration_ms,
                                 const std::string &title,
                                 const std::string &artist,
                                 const std::string &album) {
  Json::Value r;
  Json::Value lines(Json::arrayValue);
  for (const media_library::LyricLine &line : lyrics.lines) {
    Json::Value node;
    // 副字幕：LyricDto 没有独立的译文字段，客户端只渲染 `Text`。
    // 用 `_BREAK_` 把原文与译文串在同一行（与 `?format=lrc` 的正文写法一致），
    // 换行会占掉一行显示空间，也不便客户端自行拆成双行。
    // `Cues` 的下标只覆盖 `_BREAK_` 之前的原文，译文不参与下标计算。
    std::string text = media_library::line_text(line);
    const media_library::LyricItem *translation =
        media_library::find_text_item(line,
                                      media_library::LyricItemKind::Translation);
    if (translation != nullptr) {
      text += "_BREAK_" + translation->text;
    }
    node["Text"] = text;
    if (line.start_ms.has_value()) {
      node["Start"] = static_cast<Json::Int64>(*line.start_ms * 10000LL);
    }
    node["Cues"] = Json::Value(buildCues(line));
    lines.append(std::move(node));
  }

  Json::Value meta;
  meta["Title"] = title;
  if (!artist.empty()) {
    meta["Artist"] = artist;
  }
  if (!album.empty()) {
    meta["Album"] = album;
  }
  meta["Length"] = static_cast<Json::Int64>(duration_ms * 10000LL);
  meta["IsSynced"] = media_library::is_synced(lyrics);
  r["Metadata"] = std::move(meta);
  r["Lyrics"] = lines;
  return r;
}

// mm:ss.mmm（不含方括号）。分钟不封顶：LRC 允许 [123:45.678]。
std::string lrcTimestamp(std::int64_t ms) {
  if (ms < 0) {
    ms = 0;
  }
  const std::int64_t minutes = ms / 60000;
  const std::int64_t seconds = (ms / 1000) % 60;
  const std::int64_t millis = ms % 1000;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%02lld:%02lld.%03lld",
                static_cast<long long>(minutes),
                static_cast<long long>(seconds),
                static_cast<long long>(millis));
  return std::string(buf);
}

// 领域模型 → LRC 文本。门面负责序列化，媒体库层只出结构。
//
// 约定（用户给定的格式）：
//   [ar:...] [ti:...] [offset:0] 三行头部，随后每行 [mm:ss.mmm]正文。
//   同一行有译文时用 `_BREAK_` 接在正文之后：`原文_BREAK_译文`。
//   注音不导出（只取译文）；没有行时间时统一补 [00:00.000]，不伪造进度。
std::string buildLrcText(const media_library::Lyrics &lyrics,
                         const std::string &title, const std::string &artist) {
  std::string out;
  if (!artist.empty()) {
    out += "[ar:" + artist + "]\n";
  }
  if (!title.empty()) {
    out += "[ti:" + title + "]\n";
  }
  out += "[offset:0]\n";

  for (const media_library::LyricLine &line : lyrics.lines) {
    std::string text = media_library::line_text(line);
    const media_library::LyricItem *translation =
        media_library::find_text_item(line,
                                      media_library::LyricItemKind::Translation);
    if (translation != nullptr) {
      text += "_BREAK_" + translation->text;
    }
    out += "[" + lrcTimestamp(line.start_ms.value_or(0)) + "]" + text + "\n";
  }
  return out;
}

http_server::ResponsePtr plainTextResponse(int code, std::string body) {
  return http_server::textResponse(code, std::move(body),
                                   "text/plain; charset=utf-8");
}

http_server::ResponsePtr handleLyrics(const std::string &idstr,
                            const http_server::RequestPtr &req) {
  const auto ref = parse_path_ref(idstr);
  if (!ref.has_value() ||
      ref->entity_type != media_library::EntityType::Track) {
    return problem_response(404, "not_found");
  }
  const auto lyricsResult =
      media_library::media_router().resolve_lyrics(*ref, {});
  const auto trackResult =
      media_library::media_router().resolve_track(*ref, {});
  if (!lyricsResult.ok() || !trackResult.ok()) {
    return problem_response(404, "not_found");
  }
  const auto &lyrics = lyricsResult.value().value;
  if (lyrics.lines.empty()) {
    return problem_response(404, "not_found");
  }
  const auto &track = trackResult.value().value;
  const std::string artist =
      track.artists.empty() ? std::string() : track.artists.front().name;

  // 默认出 Jellyfin LyricDto（客户端按它解析）；`?format=lrc` 才出 LRC 文本。
  if (lower(qs(req, "format")) == "lrc") {
    return plainTextResponse(200, buildLrcText(lyrics, track.title, artist));
  }

  const std::string album = track.album ? track.album->title : std::string();
  return json(200, buildLyricDto(lyrics, track.duration_ms.value_or(0),
                                  track.title, artist, album));
}

http_server::ResponsePtr handleArtists(const http_server::RequestPtr &req,
                             bool album_only = false) {
  // 艺术家列表需要全库列举，路由器没有聚合 list_artists。
  (void)req;
  (void)album_only;
  return emptyList();
}

http_server::ResponsePtr handleGenres(const http_server::RequestPtr &req) {
  // 路由器没有流派枚举能力：流派的 int id 是本地库内部概念，远端无对应。
  // 按「不能靠路由器实现的不要实现」直接返回空列表。
  return emptyList();
}

http_server::ResponsePtr handleMusicGenreByName(const std::string &name) {
  // 同上：按名字取流派依赖本地流派的 int id，路由器不提供。
  (void)name;
  return problem_response(404, "not_found");
}

// ------------------------------------------------------------ 只读扩展接口

// 播放信息：返回单个 MediaSource，供客户端直接播放原始文件。
http_server::ResponsePtr handlePlaybackInfo(const http_server::RequestPtr &,
                                  const std::string &idstr) {
  const auto ref = parse_path_ref(idstr);
  if (!ref.has_value() ||
      ref->entity_type != media_library::EntityType::Track) {
    return problem_response(404, "not_found");
  }
  const auto service = media_library::playback_service_ptr();
  if (service == nullptr) {
    return stream_http::error_response_for(
        stream_http::Protocol::Jellyfin,
        media_library::Error::make(media_library::ErrorCode::ProviderUnavailable,
                                   "media runtime is not initialized"));
  }
  // 用会话拿一次摘要，随后立刻关闭：这里只需要「能不能播、多大、多久」。
  auto session = service->open_playback(*ref, media_library::CallContext{});
  if (!session.ok()) {
    return stream_http::error_response_for(stream_http::Protocol::Jellyfin,
                                          session.error());
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
  Json::Value src;
  src["Id"] = idstr;
  src["Name"] = t.title;
  // locator 是进程内定位符，绝不能出现在响应里（5.4.4）。Path 用相对播放地址。
  src["Path"] = url;
  src["Container"] = t.audio.format;
  src["Size"] = t.audio.size_bytes.value_or(0);
  src["Bitrate"] = static_cast<Json::Int64>(bitrate_bps);
  src["RunTimeTicks"] = static_cast<Json::Int64>(durationMs * 10000LL);
  src["SupportsDirectPlay"] = true;
  src["SupportsDirectStream"] = true;
  src["SupportsTranscoding"] = false;
  src["IsRemote"] = false;
  src["Type"] = "Default";
  src["DirectStreamUrl"] = url;
  src["TranscodingUrl"] = url;

  Json::Value stream;
  stream["Type"] = "Audio";
  stream["Codec"] = t.audio.format;
  stream["Index"] = 0;
  stream["IsDefault"] = true;
  stream["Channels"] = t.audio.channels.value_or(0);
  stream["SampleRate"] = t.audio.sample_rate.value_or(0);
  stream["BitRate"] = static_cast<Json::Int64>(bitrate_bps);
  Json::Value streams(Json::arrayValue);
  streams.append(std::move(stream));
  src["MediaStreams"] = streams;

  Json::Value sources(Json::arrayValue);
  sources.append(std::move(src));
  Json::Value r;
  r["MediaSources"] = sources;
  r["PlaySessionId"] = "musicd-session";
  return json(200, r);
}

// 仅支持 Primary 图片，其它图片类型返回 404。
void handleImageType(const http_server::RequestPtr &req,
                     const std::string &idstr, const std::string &imageType,
                     const std::string &, AsyncCallback callback) {
  if (lower(imageType) != "primary") {
    callback(problem_response(404, "not_found"));
    return;
  }
  handleImage(req, idstr, std::move(callback));
}

// 相似条目：同专辑 + 同流派的其它曲目。
http_server::ResponsePtr handleSimilar(const std::string &idstr) {
  // 路由器没有「相似曲目」能力，不做本地特例实现。
  (void)idstr;
  return emptyList();
}

// 下载原始文件（与 /Items/<id>/File 同义）。
void handleDownload(const http_server::RequestPtr &req,
                    const std::string &idstr, AsyncCallback callback) {
  const auto ref = parse_path_ref(idstr);
  if (!ref.has_value() ||
      ref->entity_type != media_library::EntityType::Track) {
    callback(problem_response(404, "not_found"));
    return;
  }
  // 下载与在线播放走同一条路径：字节来源一致，Range 语义一致。
  stream_http::serve_track_async(req, *ref, req->isHead(),
                                 stream_http::Protocol::Jellyfin,
                                 std::move(callback));
}

// 搜索建议：曲目 / 专辑 / 艺术家。
http_server::ResponsePtr handleSearchHints(const http_server::RequestPtr &req) {
  // typeahead 是前缀匹配，SearchQuery 只有包含匹配；语义不等价，不实现。
  (void)req;
  return emptyList();
}

// 过滤选项：真实流派与年份。
http_server::ResponsePtr handleFilters(const http_server::RequestPtr &) {
  // 流派枚举无路由器能力；年份需要遍历全部曲目，同样没有聚合入口。
  // 保留响应形状，内容为空。
  Json::Value r;
  r["Genres"] = Json::Value(Json::Value(Json::arrayValue));
  r["Tags"] = Json::Value(Json::Value(Json::arrayValue));
  r["OfficialRatings"] = Json::Value(Json::Value(Json::arrayValue));
  r["Years"] = Json::Value(Json::Value(Json::arrayValue));
  return json(200, r);
}

// 按稳定 id（或名字）取单个艺术家。
http_server::ResponsePtr handleArtistByName(const std::string &raw) {
  // 按名字精确取艺术家不是路由器能力（SearchQuery 只有包含匹配）。
  (void)raw;
  return problem_response(404, "not_found");
}

// 相似艺术家：与本艺术家共享流派的其它艺术家。
http_server::ResponsePtr handleArtistSimilar(const std::string &raw) {
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
std::vector<std::string> qsList(const http_server::RequestPtr &req,
                                const char *key) {
  return http_server::Query(req).list(std::string(key));
}

// 合并某 query 参数的全部出现（逗号连接）。
std::string qsJoined(const http_server::RequestPtr &req, const char *key) {
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
void appendBodyIds(const Json::Value &body, const char *name,
                   std::vector<int> &out) {
  if (body.isNull() || !body.isMember(name) || !body[name].isArray()) {
    return;
  }
  for (const auto &x : body[name]) {
    if (!x.isString()) {
      continue;
    }
    const auto ref = parseItemId(std::string(x.asString()));
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

// 歌单列表（跨库汇总，只经播放列表总管）。
http_server::ResponsePtr handlePlaylists(const http_server::RequestPtr &req) {
  return playlistListPage(qsInt(req, "startIndex", 0), qsInt(req, "limit", 0));
}

// 歌单详情。歌单 id 就是 SourceRef 的规范字符串，直接解析后交给总管；
// 不遍历库、不回退、不换源。
http_server::ResponsePtr handlePlaylistById(const std::string &idstr) {
  const std::shared_ptr<media_library::PlaylistDirectory> directory =
      media_library::playlist_directory_ptr();
  const auto ref = parse_path_ref(idstr);
  if (directory == nullptr || !ref.has_value() ||
      ref->entity_type != media_library::EntityType::Playlist) {
    return problem_response(404, "not_found");
  }
  const auto result = directory->get_playlist(*ref, {});
  if (!result.ok()) {
    return problem_response(404, "not_found");
  }
  return json(200, playlistDto(result.value().value, itemId(*ref)));
}

// 歌单内的曲目。曲目随歌单一起返回（Playlist.tracks 是完整领域模型），
// 不再逐条回查。
http_server::ResponsePtr handlePlaylistItems(const std::string &idstr) {
  const std::shared_ptr<media_library::PlaylistDirectory> directory =
      media_library::playlist_directory_ptr();
  const auto ref = parse_path_ref(idstr);
  if (directory == nullptr || !ref.has_value() ||
      ref->entity_type != media_library::EntityType::Playlist) {
    return emptyList();
  }
  const auto result = directory->get_playlist(*ref, {});
  if (!result.ok()) {
    return emptyList();
  }
  std::vector<Json::Value> items;
  items.reserve(result.value().value.tracks.size());
  for (const auto &track : result.value().value.tracks) {
    items.push_back(remoteTrackItem(track));
  }
  return json(200, queryResult(std::move(items),
                               static_cast<int>(items.size()), 0));
}

// 新建歌单（name/ids 可来自 query 或 JSON body）。
http_server::ResponsePtr handleCreatePlaylist(const http_server::RequestPtr &req) {
  // 歌单写入没有路由器能力（PlaylistWriter 不存在，PlaylistWrite
  // 能力从未注册）。
  (void)req;
  return problem_response(501, "not_implemented");
}

// 向歌单添加曲目（ids 可来自 query 或 JSON body）。
http_server::ResponsePtr handleAddToPlaylist(const http_server::RequestPtr &req,
                                   const std::string &idstr) {
  (void)req;
  (void)idstr;
  return problem_response(501, "not_implemented");
}

// 从歌单移除曲目（entryIds / ids 均按曲目 GUID 处理，可来自 query 或 body）。
http_server::ResponsePtr handleRemoveFromPlaylist(const http_server::RequestPtr &req,
                                        const std::string &idstr) {
  (void)req;
  (void)idstr;
  return problem_response(501, "not_implemented");
}

// 更新歌单：改名与/或替换曲目（name/ids 可来自 query 或 body）。
http_server::ResponsePtr handleUpdatePlaylist(const http_server::RequestPtr &req,
                                    const std::string &idstr) {
  (void)req;
  (void)idstr;
  return problem_response(501, "not_implemented");
}

// 删除歌单。
http_server::ResponsePtr handleDeletePlaylist(const std::string &idstr) {
  (void)idstr;
  return problem_response(501, "not_implemented");
}

} // namespace

#define JF_ROUTE(app, url) RouteBuilder((cfg.jellyfin_port), (url))

void mount(const config::Config &cfg) {
  setServerName(cfg.jellyfin_server_name);


  // 系统
  JF_ROUTE(app, "/System/Info/Public")
  ([] { return json(200, publicSystemInfo()); });
  JF_ROUTE(app, "/System/Info")([](const http_server::RequestPtr &) { return json(200, systemInfo()); });
  JF_ROUTE(app, "/System/Ping")
  ([] { return http_server::textResponse(200, "Jellyfin Server"); });
  JF_ROUTE(app, "/System/Ping")
      .methods(drogon::Post)([](const http_server::RequestPtr &) {
        return http_server::textResponse(200, "Jellyfin Server");
      });
  JF_ROUTE(app, "/System/Endpoint")([](const http_server::RequestPtr &) {
    Json::Value r;
    r["IsLocal"] = true;
    r["IsInNetwork"] = true;
    return json(200, r);
  });

  // 用户/鉴权
  JF_ROUTE(app, "/Users/Public")([](const http_server::RequestPtr &) {
    std::vector<Json::Value> items;
    items.push_back(userDto());
    Json::Value arr(Json::arrayValue);
    for (Json::Value &item : items) {
      arr.append(std::move(item));
    }
    return json(200, arr);
  });
  JF_ROUTE(app, "/Users/AuthenticateByName")
      .methods(drogon::Post)([](const http_server::RequestPtr &req) {
        std::string client;
        const Json::Value body = json_util::parseOrNull(req->body());
        if (!body.isNull() && body.isMember("Username")) {
          client = body["Username"].asString();
        }
        Json::Value r;
        r["User"] = userDto();
        r["SessionInfo"] = sessionInfo(client, "");
        r["AccessToken"] = "musicd-token";
        r["ServerId"] = serverId();
        return json(200, r);
      });
  JF_ROUTE(app, "/Users/Me")([](const http_server::RequestPtr &) { return json(200, userDto()); });
  JF_ROUTE(app, "/Users/<string>")
  ([](const http_server::RequestPtr &, std::string) { return json(200, userDto()); });

  // 视图
  JF_ROUTE(app, "/UserViews")([](const http_server::RequestPtr &) { return userViews(); });
  JF_ROUTE(app, "/Users/<string>/Views")
  ([](const http_server::RequestPtr &, std::string) { return userViews(); });

  // 条目
  JF_ROUTE(app, "/Items/Latest")
  ([](const http_server::RequestPtr &req) { return handleLatest(req); });
  JF_ROUTE(app, "/Items/Filters")
  ([](const http_server::RequestPtr &req) { return handleFilters(req); });
  JF_ROUTE(app, "/Users/<string>/Items/Latest")
  ([](const http_server::RequestPtr &req, std::string) { return handleLatest(req); });
  JF_ROUTE(app, "/Items")
  ([](const http_server::RequestPtr &req) { return handleItems(req); });
  JF_ROUTE(app, "/Users/<string>/Items")
  ([](const http_server::RequestPtr &req, std::string) { return handleItems(req); });
  JF_ROUTE(app, "/Users/<string>/Items/<string>")
  ([](const http_server::RequestPtr &, std::string, std::string itemId) {
    return handleItem(itemId);
  });
  JF_ROUTE(app, "/Items/<string>/PlaybackInfo")
      .methods(drogon::Get, drogon::Post)(
          [](const http_server::RequestPtr &req, std::string itemId) {
            return handlePlaybackInfo(req, itemId);
          });
  JF_ROUTE(app, "/Items/<string>/Images/<string>/<string>")
      .methods(drogon::Get, drogon::Head)
      .async([](const http_server::RequestPtr &req, AsyncCallback callback,
                std::string itemId, std::string imageType,
                std::string imageIndex) {
        handleImageType(req, itemId, imageType, imageIndex,
                        std::move(callback));
      });
  JF_ROUTE(app, "/Items/<string>/Images/<string>")
      .methods(drogon::Get, drogon::Head)
      .async([](const http_server::RequestPtr &req, AsyncCallback callback,
                std::string itemId, std::string imageType) {
        handleImageType(req, itemId, imageType, "", std::move(callback));
      });
  JF_ROUTE(app, "/Items/<string>/Download")
      .methods(drogon::Get, drogon::Head)
      .async([](const http_server::RequestPtr &req, AsyncCallback callback,
                std::string itemId) {
        handleDownload(req, itemId, std::move(callback));
      });
  JF_ROUTE(app, "/Items/<string>/File")
      .methods(drogon::Get, drogon::Head)
      .async([](const http_server::RequestPtr &req, AsyncCallback callback,
                std::string itemId) {
        handleDownload(req, itemId, std::move(callback));
      });
  JF_ROUTE(app, "/Items/<string>/Similar")
  ([](const http_server::RequestPtr &, std::string itemId) {
    return handleSimilar(itemId);
  });
  JF_ROUTE(app, "/Items/<string>")
  ([](const http_server::RequestPtr &, std::string itemId) {
    return handleItem(itemId);
  });
  // 通用删除项（官方 DeleteItem）：目前仅支持删除歌单，其他类型返回 404。
  JF_ROUTE(app, "/Items/<string>")
      .methods(drogon::Delete)(
          [](const http_server::RequestPtr &, std::string itemId) {
            return handleDeletePlaylist(itemId);
          });

  // 流媒体
  JF_ROUTE(app, "/Audio/<string>/Lyrics")
  ([](const http_server::RequestPtr &req, std::string itemId) {
    return handleLyrics(itemId, req);
  });
  JF_ROUTE(app, "/Audio/<string>/<string>")
      .methods(drogon::Get, drogon::Head)
      .async([](const http_server::RequestPtr &req, AsyncCallback callback,
                std::string itemId, std::string) {
        handleStream(req, itemId, std::move(callback));
      });

  // 艺术家/流派
  JF_ROUTE(app, "/Artists")
  ([](const http_server::RequestPtr &req) { return handleArtists(req); });
  JF_ROUTE(app, "/Artists/AlbumArtists")
  ([](const http_server::RequestPtr &req) { return handleArtists(req, true); });
  JF_ROUTE(app, "/Artists/<string>/Images/<string>")
      .methods(drogon::Get, drogon::Head)
      .async([](const http_server::RequestPtr &req, AsyncCallback callback,
                std::string name, std::string imageType) {
        handleImageType(req, name, imageType, "", std::move(callback));
      });
  JF_ROUTE(app, "/Artists/<string>/Similar")
  ([](const http_server::RequestPtr &, std::string name) {
    return handleArtistSimilar(name);
  });
  JF_ROUTE(app, "/Artists/<string>")
  ([](const http_server::RequestPtr &, std::string name) {
    return handleArtistByName(name);
  });
  JF_ROUTE(app, "/MusicGenres")
  ([](const http_server::RequestPtr &req) { return handleGenres(req); });
  JF_ROUTE(app, "/MusicGenres/<string>")
  ([](const http_server::RequestPtr &, std::string name) {
    return handleMusicGenreByName(http_util::urlDecode(name));
  });
  JF_ROUTE(app, "/Genres")
  ([](const http_server::RequestPtr &req) { return handleGenres(req); });
  JF_ROUTE(app, "/Genres/<string>")
  ([](const http_server::RequestPtr &, std::string name) {
    return handleMusicGenreByName(http_util::urlDecode(name));
  });
  JF_ROUTE(app, "/Studios")
  ([](const http_server::RequestPtr &) {
    std::vector<Json::Value> empty;
    return json(200, queryResult(std::move(empty), 0, 0));
  });
  JF_ROUTE(app, "/Studios/<string>")
  ([](const http_server::RequestPtr &, std::string) {
    std::vector<Json::Value> empty;
    return json(200, queryResult(std::move(empty), 0, 0));
  });

  // 播放会话
  JF_ROUTE(app, "/Sessions")([](const http_server::RequestPtr &) { return emptyList(); });
  JF_ROUTE(app, "/Sessions/Playing")
      .methods(drogon::Post)(
          [](const http_server::RequestPtr &) { return http_server::textResponse(204, ""); });
  JF_ROUTE(app, "/Sessions/Playing/Progress")
      .methods(drogon::Post)(
          [](const http_server::RequestPtr &) { return http_server::textResponse(204, ""); });
  JF_ROUTE(app, "/Sessions/Playing/Stopped")
      .methods(drogon::Post)(
          [](const http_server::RequestPtr &) { return http_server::textResponse(204, ""); });

  // 收藏
  JF_ROUTE(app, "/UserFavoriteItems/<string>")
      .methods(drogon::Post)([](const http_server::RequestPtr &, std::string) {
        Json::Value r;
        r["IsFavorite"] = true;
        r["PlayCount"] = 0;
        r["Played"] = false;
        r["Key"] = "";
        return json(200, r);
      });
  JF_ROUTE(app, "/UserFavoriteItems/<string>")
      .methods(drogon::Delete)(
          [](const http_server::RequestPtr &, std::string) {
            Json::Value r;
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
  ([](const http_server::RequestPtr &, std::string id) {
    return json(200, displayPreferences(id));
  });
  JF_ROUTE(app, "/DisplayPreferences/<string>")
      .methods(drogon::Post)([](const http_server::RequestPtr &, std::string) {
        return http_server::textResponse(204, "");
      });
  JF_ROUTE(app, "/ScheduledTasks")([](const http_server::RequestPtr &) { return emptyList(); });
  JF_ROUTE(app, "/Plugins")([](const http_server::RequestPtr &) { return emptyList(); });
  JF_ROUTE(app, "/Search/Hints")
  ([](const http_server::RequestPtr &req) { return handleSearchHints(req); });
  JF_ROUTE(app, "/Shows/NextUp")([](const http_server::RequestPtr &) {
    std::vector<Json::Value> empty;
    return json(200, queryResult(std::move(empty), 0, 0));
  });
  JF_ROUTE(app, "/UserItems/Resume")([](const http_server::RequestPtr &) {
    std::vector<Json::Value> empty;
    return json(200, queryResult(std::move(empty), 0, 0));
  });
  JF_ROUTE(app, "/Playlists")([](const http_server::RequestPtr &req) {
    return handlePlaylists(req);
  });
  JF_ROUTE(app, "/Playlists")
      .methods(drogon::Post)(
          [](const http_server::RequestPtr &req) { return handleCreatePlaylist(req); });
  JF_ROUTE(app, "/Playlists/<string>")
  ([](const http_server::RequestPtr &, std::string id) {
    return handlePlaylistById(id);
  });
  JF_ROUTE(app, "/Playlists/<string>")
      .methods(drogon::Post)(
          [](const http_server::RequestPtr &req, std::string id) {
            return handleUpdatePlaylist(req, id);
          });
  JF_ROUTE(app, "/Playlists/<string>")
      .methods(drogon::Delete)(
          [](const http_server::RequestPtr &, std::string id) {
            return handleDeletePlaylist(id);
          });
  JF_ROUTE(app, "/Playlists/<string>/Items")
  ([](const http_server::RequestPtr &, std::string id) {
    return handlePlaylistItems(id);
  });
  JF_ROUTE(app, "/Playlists/<string>/Items")
      .methods(drogon::Post)(
          [](const http_server::RequestPtr &req, std::string id) {
            return handleAddToPlaylist(req, id);
          });
  JF_ROUTE(app, "/Playlists/<string>/Items")
      .methods(drogon::Delete)(
          [](const http_server::RequestPtr &req, std::string id) {
            return handleRemoveFromPlaylist(req, id);
          });
  JF_ROUTE(app, "/Localization/Options")([](const http_server::RequestPtr &) {
    Json::Value arr;
    arr = Json::Value(Json::arrayValue);
    return json(200, arr);
  });
  JF_ROUTE(app, "/Users/<string>/GroupingOptions")
  ([](const http_server::RequestPtr &, std::string) {
    Json::Value r;
    r["GroupingOptions"] = Json::Value(Json::arrayValue);
    return json(200, r);
  });
  JF_ROUTE(app, "/QuickConnect/Enabled")([](const http_server::RequestPtr &) {
    Json::Value r;
    r = false;
    return json(200, r);
  });
  JF_ROUTE(app, "/System/Configuration")([](const http_server::RequestPtr &) {
    Json::Value r;
    return json(200, r);
  });
  JF_ROUTE(app, "/Users/<string>/Configuration")
  ([](const http_server::RequestPtr &, std::string) {
    Json::Value r;
    return json(200, r);
  });

  // Drogon 的路径匹配本身大小写不敏感（正则 icase），
  // 参数值保持原样——Jellyfin 客户端会以各种大小写请求路径。
  http_server::setQuietLogging();
}

} // namespace jellyfin
