#include "api/subsonic/subsonic.h"

#include "api/subsonic/access_log.h"
#include "api/subsonic/dto.h"
#include "api/subsonic/envelope.h"
#include "api/subsonic/params.h"

#include "http_util.h"
#include "media_library/library_directory.h"
#include "media_library/media_runtime.h"
#include "media_library/source_ref.h"
#include "http_server.h"
#include "stream_http.h"

#include <algorithm>
#include <functional>
#include <json/json.h>
#include <memory>
#include <optional>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace subsonic {
namespace {

using media_library::Album;
using media_library::AlbumInfo;
using media_library::AlbumRef;
using media_library::Artist;
using media_library::ArtistInfo;
using media_library::ArtistRef;
using media_library::CallContext;
using media_library::EntityType;
using media_library::Error;
using media_library::ErrorCode;
using media_library::PageRequest;
using media_library::Playlist;
using media_library::PlaylistRef;
using media_library::SourceRef;
using media_library::Track;
using media_library::TrackRef;

// 引用类型都是 SourceRef 的别名，这里只做语义转换（AlbumArtist -> Artist）。
AlbumRef album_ref_of(const SourceRef &ref) { return ref; }
ArtistRef artist_ref_of(const SourceRef &ref) { return ref; }
TrackRef track_ref_of(const SourceRef &ref) { return ref; }
PlaylistRef playlist_ref_of(const SourceRef &ref) { return ref; }

constexpr const char *kServerVersion = "0.1.0";

// Subsonic search2/search3 的规范默认值是 20；单类最多回 100 条。
constexpr int kSearchDefaultCount = 20;
constexpr int kSearchMaxCount = 100;
constexpr int kSearchMaxOffset = 100000;
// getAlbum/getArtist 要完整列表：本地库按 limit 截断，网易云歌手专辑
// 内部 clamp 到 100 并给 next_cursor，所以这里用大 limit + 游标循环。
constexpr int kFetchAllPageSize = 1000000;
constexpr int kFetchAllMaxPages = 1000;

// 一次请求的公共上下文：服务器名与是否记日志。
struct Session {
  std::string server_name;
  std::string server_version;
  bool log_requests = true;
  int default_page_size = 50;
  // `*` = 回显任意 Origin；`a,b` = 白名单；空 = 关闭 CORS。
  std::string cors_allow_origin{"*"};
  // 监听端口：路由注册与入端口判定都靠它。
  int port = 0;
};

Session g_session;

// ---------------------------------------------------------------------------
// CORS
//
// 浏览器里跑的 Web 客户端（典型是 https://localhost 的页面）直接打 4533 是**跨源**，
// 响应缺 Access-Control-Allow-Origin 时浏览器会把响应当作不可读，前端只看到
// "TypeError: Failed to fetch" —— 与服务端返回码无关，光看后端日志会以为一切正常。
//
// 三个实现要点：
// 1) 走 **post-handling advice** 而不是各 handler 自己加头，一次覆盖本端口全部
//    响应。Drogon 默认会在路由前自行短路 OPTIONS（只回 Allow 头，且不会进入
//    post-handling）；我们对本服务端口的 `/rest/*` 预检打上
//    `drogon.customCORShandling` 标记，把请求交回路由，预检响应因此也能经过
//    同一条 advice。
// 2) 没有 Origin 头就不加任何 CORS 头，原生客户端（curl/手机 App）响应逐字节不变。
// 3) 认证全在 query string（u/t/s/p），不用 cookie，所以不必发
//    Access-Control-Allow-Credentials；回显 Origin 即可。

// 回显值的来源：`*`（默认）= 回显任意来源；`a,b` = 白名单；空 = 关闭 CORS。
std::string cors_origin_of(const std::string &origin) {
  const std::string &configured = g_session.cors_allow_origin;
  if (configured.empty()) {
    return std::string();
  }
  if (configured == "*") {
    return origin;
  }
  std::size_t begin = 0;
  while (begin <= configured.size()) {
    std::size_t end = configured.find(',', begin);
    if (end == std::string::npos) {
      end = configured.size();
    }
    std::size_t from = configured.find_first_not_of(" \t", begin);
    std::size_t to = configured.find_last_not_of(" \t", end == 0 ? 0 : end - 1);
    if (from != std::string::npos && to != std::string::npos && to >= from &&
        configured.compare(from, to - from + 1, origin) == 0) {
      return origin;
    }
    if (end == configured.size()) {
      break;
    }
    begin = end + 1;
  }
  return std::string();
}

// 回显 Access-Control-Request-Headers 前先洗一遍：响应头里绝不能出现 CR/LF。
std::string cors_sanitize(const std::string &value) {
  std::string cleaned;
  cleaned.reserve(value.size());
  for (const char ch : value) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c >= 0x20 && c < 0x7f) {
      cleaned.push_back(ch);
    }
  }
  return cleaned;
}

void apply_cors(const http_server::RequestPtr &req,
                const http_server::ResponsePtr &res) {
  const std::string &configured = g_session.cors_allow_origin;
  if (configured.empty()) {
    return;  // 显式关闭。
  }

  if (req->method() == drogon::Options) {
    // 预检现在会经过路由与 post-handling advice，因此理论上能拿到 Origin；
    // 但白名单模式沿用旧策略：预检不按来源回显，直接不给 CORS 头（宁可失败
    // 也不 fail-open）。浏览器随后会拦下这个预检，真正的响应仍严格按白名单回显。
    if (configured != "*") {
      return;
    }
    res->addHeader("Access-Control-Allow-Origin", "*");
    res->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS, HEAD");
    // 预检允许任意自定义头；安全性不靠这里：凭据在 query string，
    // 且非预检响应仍按 Origin 白名单收口。
    res->addHeader("Access-Control-Allow-Headers", "*");
    res->addHeader("Access-Control-Max-Age", "86400");
    return;
  }

  const std::string origin = req->getHeader("Origin");
  if (origin.empty()) {
    return;  // 原生客户端：响应逐字节不变。
  }
  const std::string echoed = cors_origin_of(origin);
  if (echoed.empty()) {
    return;
  }
  res->addHeader("Access-Control-Allow-Origin", echoed);
  res->addHeader("Vary", "Origin");
  // 浏览器默认读不到这些头；Web 播放器靠 Content-Range 算时长和拖动进度。
  res->addHeader("Access-Control-Expose-Headers",
                 "Content-Length, Content-Range, Accept-Ranges, Content-Type");
}

// 构造一条成功响应。
http_server::ResponsePtr ok(Json::Value payload) {
  return ok_response(std::move(payload), g_session.server_name,
                     g_session.server_version);
}

// 解析实体 id。失败时返回 nullopt，由调用方翻译成 NotFound。
std::optional<SourceRef> parse_id(const std::string &text,
                                  std::optional<EntityType> expect) {
  const std::optional<SourceRef> ref = media_library::parse_ref(text);
  if (!ref.has_value()) {
    return std::nullopt;
  }
  if (expect.has_value() && ref->entity_type != *expect) {
    return std::nullopt;
  }
  return ref;
}

PageRequest page_of(const Paging &paging) {
  PageRequest page;
  page.limit = paging.size;
  if (paging.offset > 0) {
    page.cursor = std::to_string(paging.offset);
  }
  return page;
}

// ---------------------------------------------------------------------------
// 基础端点
// ---------------------------------------------------------------------------

http_server::ResponsePtr handle_ping(const http_server::RequestPtr &) { return ok(Json::Value(Json::objectValue)); }

http_server::ResponsePtr handle_license(const http_server::RequestPtr &) {
  Json::Value payload;
  Json::Value license;
  license["valid"] = true;
  license["email"] = "anonymous@localhost";
  license["licenseExpires"] = "2099-12-31T23:59:59";
  payload["license"] = std::move(license);
  return ok(std::move(payload));
}

// OpenSubsonic 扩展声明：客户端（Feishin 等）靠这个判断服务端是否支持
// getLyricsBySongId 的结构化歌词及 v2 的 enhanced 字段。只宣告真正实现的扩展。
http_server::ResponsePtr handle_open_subsonic_extensions(const http_server::RequestPtr &) {
  Json::Value entry;
  entry["name"] = "songLyrics";
  entry["versions"] = Json::Value(Json::arrayValue);
  entry["versions"].append(1);
  entry["versions"].append(2);
  Json::Value extensions(Json::arrayValue);
  extensions.append(std::move(entry));
  Json::Value payload;
  payload["openSubsonicExtensions"] =
      std::move(extensions);
  return ok(std::move(payload));
}

http_server::ResponsePtr handle_music_folders(const http_server::RequestPtr &) {
  const std::shared_ptr<media_library::LibraryRegistry> registry =
      media_library::library_registry_ptr();
  Json::Value folders(Json::arrayValue);
  if (registry != nullptr) {
    media_library::LibraryDirectory directory(registry);
    for (const media_library::LibraryView &view : directory.list()) {
      if (!view.enabled) {
        continue;
      }
      folders.append(music_folder_dto(view.library_id, view.display_name));
    }
  }
  Json::Value payload;
  payload["musicFolders"] = Json::Value(Json::objectValue);
  payload["musicFolders"]["musicFolder"] = std::move(folders);
  return ok(std::move(payload));
}

// ---------------------------------------------------------------------------
// 账号
// ---------------------------------------------------------------------------

// 本服务不校验账号：只回一个合成用户，用户名取客户端传来的值。
// getUsers 没有 username 参数，退回凭据参数 u；两者都空时用 "admin"。
http_server::ResponsePtr handle_user(const http_server::RequestPtr &req) {
  Json::Value payload;
  payload["user"] = user_dto(text(req, "username"));
  return ok(std::move(payload));
}

http_server::ResponsePtr handle_users(const http_server::RequestPtr &req) {
  Json::Value payload;
  Json::Value users(Json::arrayValue);
  users.append(user_dto(text(req, "u")));
  payload["users"] = Json::Value(Json::objectValue);
  payload["users"]["user"] = std::move(users);
  return ok(std::move(payload));
}

// ---------------------------------------------------------------------------
// 单实体
// ---------------------------------------------------------------------------

http_server::ResponsePtr handle_song(const http_server::RequestPtr &req) {
  const std::shared_ptr<media_library::MediaRouter> router =
      media_library::media_router_ptr();
  const std::optional<std::string> raw = id_param(req);
  if (!raw.has_value()) {
    return failure_response(Code::MissingParameter, "id is required");
  }
  const std::optional<SourceRef> ref = parse_id(*raw, EntityType::Track);
  if (!ref.has_value()) {
    return failure_response(Code::NotFound, "song not found: " + *raw);
  }
  if (router == nullptr) {
    return failure_response(Code::Ok, "media runtime is not initialized");
  }
  media_library::Result<media_library::Sourced<Track>> result =
      router->get_track(*ref, CallContext{});
  if (!result.ok()) {
    return failure_response(code_of(result.error().code),
                            error_name(result.error().code));
  }
  // Subsonic 的曲目必须挂在 subsonic-response.song 下；ok_response 会把
  // object 型 payload 逐键展开，所以这里必须显式包一层，否则客户端拿不到
  // song 对象（连带 albumId / artistId 一起失效，点专辑、点歌手都没反应）。
  Json::Value payload;
  payload["song"] = song_dto(result.value().value);
  return ok(std::move(payload));
}

http_server::ResponsePtr handle_album(const http_server::RequestPtr &req) {
  const std::shared_ptr<media_library::MediaRouter> router =
      media_library::media_router_ptr();
  const std::optional<std::string> raw = id_param(req);
  if (!raw.has_value()) {
    return failure_response(Code::MissingParameter, "id is required");
  }
  const std::optional<SourceRef> ref = parse_id(*raw, EntityType::Album);
  if (!ref.has_value()) {
    return failure_response(Code::NotFound, "album not found: " + *raw);
  }
  if (router == nullptr) {
    return failure_response(Code::Ok, "media runtime is not initialized");
  }
  const AlbumRef album_ref = album_ref_of(*ref);
  media_library::Result<media_library::Sourced<Album>> album =
      router->get_album(album_ref, CallContext{});
  if (!album.ok()) {
    return failure_response(code_of(album.error().code),
                            error_name(album.error().code));
  }

  Json::Value node = album_dto(album.value().value, true);

  // song[] 与 songCount / duration 都来自同一批 list_album_tracks。
  // 本地库给大 limit 会一次返回全部；网易云若还有 next_cursor 就继续跟。
  PageRequest page;
  page.limit = kFetchAllPageSize;
  Json::Value songs(Json::arrayValue);
  long long total_ms = 0;
  std::optional<std::string> cursor;
  for (int round = 0; round < kFetchAllMaxPages; ++round) {
    page.cursor = cursor;
    media_library::Result<media_library::Sourced<media_library::Page<Track>>>
        tracks = router->list_album_tracks(album_ref, page, CallContext{});
    if (!tracks.ok()) {
      break;
    }
    const media_library::Page<Track> &value = tracks.value().value;
    for (const auto &item : value.items) {
      total_ms += item.duration_ms.value_or(0);
      songs.append(song_dto(item));
    }
    if (!value.has_more || !value.next_cursor.has_value()) {
      break;
    }
    if (cursor.has_value() && *value.next_cursor == *cursor) {
      break;
    }
    cursor = value.next_cursor;
  }
  node["songCount"] = static_cast<int>(songs.size());
  node["duration"] = static_cast<int>(total_ms / 1000);
  node["song"] = std::move(songs);

  Json::Value payload;
  payload["album"] = std::move(node);
  return ok(std::move(payload));
}

http_server::ResponsePtr handle_artist(const http_server::RequestPtr &req) {
  const std::shared_ptr<media_library::MediaRouter> router =
      media_library::media_router_ptr();
  const std::optional<std::string> raw = id_param(req);
  if (!raw.has_value()) {
    return failure_response(Code::MissingParameter, "id is required");
  }
  const std::optional<SourceRef> parsed = media_library::parse_ref(*raw);
  if (!parsed.has_value() ||
      (parsed->entity_type != EntityType::Artist &&
       parsed->entity_type != EntityType::AlbumArtist)) {
    return failure_response(Code::NotFound, "artist not found: " + *raw);
  }
  if (router == nullptr) {
    return failure_response(Code::Ok, "media runtime is not initialized");
  }
  // album.artistId 可能指向 album_artist，两者都要能取到同一个艺术家。
  const SourceRef &ref = *parsed;
  const ArtistRef artist_ref = ref.entity_type == EntityType::AlbumArtist
                                   ? media_library::as_artist_ref(ref)
                                   : artist_ref_of(ref);
  media_library::Result<media_library::Sourced<Artist>> artist =
      router->get_artist(artist_ref, CallContext{});
  if (!artist.ok()) {
    return failure_response(code_of(artist.error().code),
                            error_name(artist.error().code));
  }

  PageRequest page;
  page.limit = kFetchAllPageSize;
  Json::Value album_list(Json::arrayValue);
  std::optional<std::string> cursor;
  for (int round = 0; round < kFetchAllMaxPages; ++round) {
    page.cursor = cursor;
    media_library::Result<media_library::Sourced<media_library::Page<Album>>>
        albums = router->list_artist_albums(artist_ref, page, CallContext{});
    if (!albums.ok()) {
      break;
    }
    const media_library::Page<Album> &value = albums.value().value;
    for (const auto &item : value.items) {
      album_list.append(album_dto(item, false));
    }
    if (!value.has_more || !value.next_cursor.has_value()) {
      break;
    }
    if (cursor.has_value() && *value.next_cursor == *cursor) {
      break;
    }
    cursor = value.next_cursor;
  }

  Json::Value node =
      artist_dto(artist.value().value, static_cast<int>(album_list.size()));
  node["album"] = std::move(album_list);

  Json::Value payload;
  payload["artist"] = std::move(node);
  return ok(std::move(payload));
}

// ---------------------------------------------------------------------------
// 信息（描述）出口
// ---------------------------------------------------------------------------
//
// 描述走元数据方面：与 resolve_track 同一套顺序（链接 override →
// 配置 library_priority.metadata → 内建顺序），由 Router 逐库回退。
// 取不到描述（本地库没有 InfoProvider、或远端失败）不是协议错误，按
// Subsonic 惯例回空对象；只有 id 本身不合法才回 failed 信封。

http_server::ResponsePtr handle_album_info2(const http_server::RequestPtr &req) {
  const std::shared_ptr<media_library::MediaRouter> router =
      media_library::media_router_ptr();
  const std::optional<std::string> raw = id_param(req);
  if (!raw.has_value()) {
    return failure_response(Code::MissingParameter, "id is required");
  }
  const std::optional<SourceRef> ref = parse_id(*raw, EntityType::Album);
  if (!ref.has_value()) {
    return failure_response(Code::NotFound, "album not found: " + *raw);
  }
  if (router == nullptr) {
    return failure_response(Code::Ok, "media runtime is not initialized");
  }

  AlbumInfo info;
  media_library::Result<media_library::Sourced<AlbumInfo>> resolved =
      router->resolve_album_info(album_ref_of(*ref), CallContext{});
  if (resolved.ok()) {
    info = std::move(resolved.value().value);
  }

  Json::Value payload;
  payload["albumInfo2"] = album_info_dto(info);
  return ok(std::move(payload));
}

http_server::ResponsePtr handle_artist_info2(const http_server::RequestPtr &req) {
  const std::shared_ptr<media_library::MediaRouter> router =
      media_library::media_router_ptr();
  const std::optional<std::string> raw = id_param(req);
  if (!raw.has_value()) {
    return failure_response(Code::MissingParameter, "id is required");
  }
  const std::optional<SourceRef> parsed = media_library::parse_ref(*raw);
  if (!parsed.has_value() ||
      (parsed->entity_type != EntityType::Artist &&
       parsed->entity_type != EntityType::AlbumArtist)) {
    return failure_response(Code::NotFound, "artist not found: " + *raw);
  }
  if (router == nullptr) {
    return failure_response(Code::Ok, "media runtime is not initialized");
  }

  // 不做 Artist/AlbumArtist 转换：描述查询按原始引用类型选候选，
  // 网易云两种引用都能取到同一位艺术家的简介。
  ArtistInfo info;
  media_library::Result<media_library::Sourced<ArtistInfo>> resolved =
      router->resolve_artist_info(*parsed, CallContext{});
  if (resolved.ok()) {
    info = std::move(resolved.value().value);
  }

  Json::Value payload;
  payload["artistInfo2"] = artist_info_dto(info);
  return ok(std::move(payload));
}

// ---------------------------------------------------------------------------
// 字节出口
// ---------------------------------------------------------------------------

void handle_stream(const http_server::RequestPtr &req,
                   std::function<void(const http_server::ResponsePtr &)> callback) {
  const std::optional<std::string> raw = id_param(req);
  if (!raw.has_value()) {
    callback(failure_response(Code::MissingParameter, "id is required"));
    return;
  }
  const std::optional<SourceRef> ref = parse_id(*raw, EntityType::Track);
  if (!ref.has_value()) {
    callback(failure_response(Code::NotFound, "song not found: " + *raw));
    return;
  }
  // 音频字节：本地库按实体 id 直连存储服务，远端库沿用同步路径。
  stream_http::serve_track_async(req, track_ref_of(*ref),
                                 req->isHead(),
                                 std::move(callback));
}

void handle_cover_art(const http_server::RequestPtr &req,
                      std::function<void(const http_server::ResponsePtr &)>
                          callback) {
  const std::optional<std::string> raw = id_param(req);
  if (!raw.has_value()) {
    callback(failure_response(Code::MissingParameter, "id is required"));
    return;
  }
  const std::optional<SourceRef> ref = media_library::parse_ref(*raw);
  if (!ref.has_value() ||
      (ref->entity_type != EntityType::Track &&
       ref->entity_type != EntityType::Album &&
       ref->entity_type != EntityType::Artist &&
       ref->entity_type != EntityType::AlbumArtist)) {
    callback(failure_response(Code::NotFound, "cover art not found"));
    return;
  }
  media_library::ArtworkRef artwork;
  artwork.owner = ref->entity_type == EntityType::AlbumArtist
                      ? media_library::as_artist_ref(*ref)
                      : *ref;
  stream_http::serve_artwork_async(req, artwork,
                                   req->isHead(),
                                   std::move(callback));
}

// ---------------------------------------------------------------------------
// 搜索
// ---------------------------------------------------------------------------

media_library::EntityType entity_type_of(const char *text) {
  return media_library::parse_entity_type(text)
      .value_or(EntityType::Track);
}

// search2 / search3 共用：差别只在输出结构的键名。
http_server::ResponsePtr handle_search(const http_server::RequestPtr &req, bool id3) {
  const std::shared_ptr<media_library::MediaRouter> router =
      media_library::media_router_ptr();
  if (router == nullptr) {
    return failure_response(Code::Ok, "media runtime is not initialized");
  }

  const std::string search_text = text(req, "query");
  if (search_text.empty()) {
    return failure_response(Code::MissingParameter, "query is required");
  }

  const auto count_of = [&req](const char *name) {
    return std::clamp(integer(req, name, kSearchDefaultCount), 0,
                      kSearchMaxCount);
  };
  const auto offset_of = [&req](const char *name) {
    return std::clamp(integer(req, name, 0), 0, kSearchMaxOffset);
  };
  const int song_count = count_of("songCount");
  const int album_count = count_of("albumCount");
  const int artist_count = count_of("artistCount");
  const int song_offset = offset_of("songOffset");
  const int album_offset = offset_of("albumOffset");
  const int artist_offset = offset_of("artistOffset");

  // musicFolderId：限定到单个库（参数名大小写不敏感）。
  const std::string folder = text(req, "musicFolderId");

  // 三类各自有自己的 count/offset，不能合成一次查询：Router 的 limit
  // 是整条聚合流的总条数，合查会让某一类把其它类饿死。
  const auto search_type = [&](EntityType type, int count, int offset,
                               Json::Value &out)
      -> std::optional<http_server::ResponsePtr> {
    if (count <= 0) {
      return std::nullopt;
    }
    media_library::SearchQuery query;
    query.text = search_text;
    query.types.push_back(type);
    query.page.limit = count;
    query.page.offset = offset;
    if (!folder.empty()) {
      query.library_id = folder;
    }
    media_library::Result<media_library::RoutedPage<media_library::SearchHit>>
        result = router->search(query, CallContext{});
    if (!result.ok()) {
      return failure_response(code_of(result.error().code),
                              error_name(result.error().code));
    }
    for (const auto &item : result.value().items) {
      out.append(search_hit_dto(item.value));
    }
    return std::nullopt;
  };

  Json::Value songs(Json::arrayValue);
  Json::Value albums(Json::arrayValue);
  Json::Value artists(Json::arrayValue);
  if (auto error = search_type(EntityType::Track, song_count, song_offset,
                               songs);
      error.has_value()) {
    return std::move(*error);
  }
  if (auto error = search_type(EntityType::Album, album_count, album_offset,
                               albums);
      error.has_value()) {
    return std::move(*error);
  }
  if (auto error = search_type(EntityType::Artist, artist_count, artist_offset,
                               artists);
      error.has_value()) {
    return std::move(*error);
  }

  const char *kind_album = id3 ? "albumID3" : "album";
  const char *kind_artist = id3 ? "artistID3" : "artist";

  // 集合键名随 search2/search3 变化，obj 名同理。
  Json::Value result_node;
  result_node["song"] = std::move(songs);
  result_node[kind_album] = std::move(albums);
  result_node[kind_artist] = std::move(artists);

  Json::Value payload;
  payload[id3 ? "searchResult3" : "searchResult2"] = std::move(result_node);
  return ok(std::move(payload));
}

// ---------------------------------------------------------------------------
// 歌单
// ---------------------------------------------------------------------------

http_server::ResponsePtr handle_playlists(const http_server::RequestPtr &req) {
  const std::shared_ptr<media_library::PlaylistDirectory> directory =
      media_library::playlist_directory_ptr();
  if (directory == nullptr) {
    return failure_response(Code::Ok, "media runtime is not initialized");
  }
  const Paging paging = subsonic::paging(req, g_session.default_page_size);
  media_library::Result<media_library::RoutedPage<Playlist>> result =
      directory->list_playlists(page_of(paging), CallContext{});
  if (!result.ok()) {
    return failure_response(code_of(result.error().code),
                            error_name(result.error().code));
  }
  Json::Value items(Json::arrayValue);
  for (const auto &item : result.value().items) {
    items.append(playlist_dto(item.value, false));
  }
  Json::Value payload;
  payload["playlists"] = Json::Value(Json::objectValue);
  payload["playlists"]["playlist"] = std::move(items);
  return ok(std::move(payload));
}

http_server::ResponsePtr handle_playlist(const http_server::RequestPtr &req) {
  const std::shared_ptr<media_library::PlaylistDirectory> directory =
      media_library::playlist_directory_ptr();
  const std::optional<std::string> raw = id_param(req);
  if (!raw.has_value()) {
    return failure_response(Code::MissingParameter, "id is required");
  }
  const std::optional<SourceRef> ref = parse_id(*raw, EntityType::Playlist);
  if (!ref.has_value()) {
    return failure_response(Code::NotFound, "playlist not found: " + *raw);
  }
  if (directory == nullptr) {
    return failure_response(Code::Ok, "media runtime is not initialized");
  }
  media_library::Result<media_library::Sourced<Playlist>> result =
      directory->get_playlist(playlist_ref_of(*ref),
                              CallContext{});
  if (!result.ok()) {
    return failure_response(code_of(result.error().code),
                            error_name(result.error().code));
  }
  Json::Value payload;
  payload["playlist"] = playlist_dto(result.value().value, true);
  return ok(std::move(payload));
}

// ---------------------------------------------------------------------------
// 歌词
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 歌词（OpenSubsonic songLyrics v2）
// ---------------------------------------------------------------------------

namespace {

// 一条可输出的歌词轨：正文行 + 可选的逐字时间。
struct LyricTrack {
  std::string text;
  std::optional<std::int64_t> start_ms;
  std::optional<std::int64_t> end_ms;
  std::vector<media_library::LyricWord> words;
  bool has_words = false;
};

// Subsonic 的 cue 用 **UTF-8 字节偏移** 而且是闭区间；跨行拼字时必须按字节累加，
// 不能按码点数（中日常用字都是 3 字节，混排时按码点算一定错位）。
Json::Value cue_list_of(const std::vector<media_library::LyricWord> &words,
                                     std::optional<std::int64_t> line_end_ms) {
  Json::Value cues(Json::arrayValue);

  std::string value;
  for (const auto &word : words) {
    value += word.text;
  }

  // end 的 all-or-none 契约：只要有一个词没有 end_ms，就要么全部补齐、要么全部省略。
  // 这里统一补齐——用下一个词的起点，最后一个词退化到行尾。
  bool any_end = false;
  for (const auto &word : words) {
    if (word.end_ms.has_value()) {
      any_end = true;
      break;
    }
  }

  std::size_t byte_pos = 0;
  for (std::size_t i = 0; i < words.size(); ++i) {
    const media_library::LyricWord &word = words[i];
    const std::size_t byte_start = byte_pos;
    byte_pos += word.text.size();
    const std::size_t byte_end = byte_pos == 0 ? 0 : byte_pos - 1;
    if (word.text.empty()) {
      continue;
    }

    Json::Value cue;
    cue["start"] = word.start_ms;
    if (any_end) {
      std::int64_t end = word.start_ms;
      if (word.end_ms.has_value()) {
        end = *word.end_ms;
      } else if (i + 1 < words.size()) {
        end = words[i + 1].start_ms;
      } else if (line_end_ms.has_value()) {
        end = *line_end_ms;
      }
      cue["end"] = end;
    }
    cue["byteStart"] = static_cast<int>(byte_start);
    cue["byteEnd"] = static_cast<int>(byte_end);
    cue["value"] = word.text;
    cues.append(std::move(cue));
  }
  return cues;
}

// 组装一个 structuredLyrics 元素。
// kind 只在 enhanced 时输出：main / translation / pronunciation。
Json::Value structured_lyrics(const std::string &lang, bool synced,
                                     const std::vector<LyricTrack> &tracks,
                                     const std::string &kind, bool enhanced) {
  Json::Value lines(Json::arrayValue);
  Json::Value cue_lines(Json::arrayValue);

  for (std::size_t index = 0; index < tracks.size(); ++index) {
    const LyricTrack &track = tracks[index];

    Json::Value line;
    line["value"] = track.text;
    if (synced && track.start_ms.has_value()) {
      line["start"] = *track.start_ms;
    }
    lines.append(std::move(line));

    // cueLine 只在该轨真的有逐字时间、且整体是同步歌词时才给。
    if (!enhanced || !synced || !track.has_words || track.words.empty()) {
      continue;
    }
    std::string value;
    for (const auto &word : track.words) {
      value += word.text;
    }
    Json::Value cue_line;
    cue_line["index"] = static_cast<int>(index);
    if (track.start_ms.has_value()) {
      cue_line["start"] = *track.start_ms;
    }
    if (track.end_ms.has_value()) {
      cue_line["end"] = *track.end_ms;
    }
    cue_line["value"] = value;
    cue_line["cue"] = cue_list_of(track.words, track.end_ms);
    cue_lines.append(std::move(cue_line));
  }

  Json::Value node;
  node["lang"] = lang;
  node["synced"] = synced;
  node["line"] = std::move(lines);
  if (!cue_lines.empty()) {
    node["cueLine"] = std::move(cue_lines);
  }
  if (enhanced) {
    node["kind"] = kind;
  }
  return node;
}

std::string lyric_lang(const media_library::Lyrics &lyrics) {
  if (lyrics.language.has_value() && !lyrics.language->empty()) {
    return *lyrics.language;
  }
  return "und";
}

} // namespace

http_server::ResponsePtr handle_lyrics_by_song_id(const http_server::RequestPtr &req) {
  const std::shared_ptr<media_library::MediaRouter> router =
      media_library::media_router_ptr();
  const std::optional<std::string> raw = id_param(req);
  if (!raw.has_value()) {
    return failure_response(Code::MissingParameter, "id is required");
  }
  const std::optional<SourceRef> ref = parse_id(*raw, EntityType::Track);
  if (!ref.has_value()) {
    return failure_response(Code::NotFound, "song not found: " + *raw);
  }
  if (router == nullptr) {
    return failure_response(Code::Ok, "media runtime is not initialized");
  }
  media_library::Result<media_library::Sourced<media_library::Lyrics>> result =
      router->resolve_lyrics(track_ref_of(*ref), CallContext{});
  if (!result.ok()) {
    return failure_response(code_of(result.error().code),
                            error_name(result.error().code));
  }
  const media_library::Lyrics &lyrics = result.value().value;

  // enhanced=true 才出逐字（cueLine）与副轨（translation / pronunciation）。
  // 缺省 false 时只回 kind=main 一条，与旧版客户端兼容。
  const std::string enhanced_text = text(req, "enhanced");
  const bool enhanced = enhanced_text == "true" || enhanced_text == "1" ||
                        enhanced_text == "yes";

  const bool synced = media_library::is_synced(lyrics);
  const std::string lang = lyric_lang(lyrics);

  std::vector<LyricTrack> main_tracks;
  main_tracks.reserve(lyrics.lines.size());
  for (const auto &line : lyrics.lines) {
    LyricTrack track;
    track.text = media_library::line_text(line);
    track.start_ms = line.start_ms;
    track.end_ms = line.end_ms;
    track.words = media_library::line_words(line);
    track.has_words = !track.words.empty();
    main_tracks.push_back(std::move(track));
  }

  Json::Value structured(Json::arrayValue);
  structured.append(
      structured_lyrics(lang, synced, main_tracks, "main", enhanced));

  if (enhanced) {
    // 译文与注音各自是一条独立轨（kind 区分），行时间与正文行对齐。
    const auto emit_aux = [&](media_library::LyricItemKind kind,
                              const char *name) {
      std::vector<LyricTrack> tracks;
      tracks.reserve(lyrics.lines.size());
      bool any = false;
      for (const auto &line : lyrics.lines) {
        const media_library::LyricItem *found = nullptr;
        for (const auto &item : line.items) {
          if (item.kind != kind) {
            continue;
          }
          // 同一行可能同时有空占位和真实文本，优先真实文本。
          if (found == nullptr || (found->text.empty() && !item.text.empty())) {
            found = &item;
          }
          if (!found->text.empty()) {
            break;
          }
        }
        if (found == nullptr) {
          // 该行没有对应的副轨内容：不输出，保持 OpenSubsonic 允许的
          // 「副轨与正文轨不要求 1:1」。
          continue;
        }
        LyricTrack track;
        // 空串也是有效占位：媒体库层保留的间奏位置必须原样传出去，
        // 否则客户端找不到精确时间点，会沿用上一条译文/标音。
        track.text = found->text;
        track.start_ms = line.start_ms;
        track.end_ms = line.end_ms;
        track.words = media_library::item_words(*found);
        track.has_words = !track.words.empty();
        any = true;
        tracks.push_back(std::move(track));
      }
      if (any) {
        structured.append(
            structured_lyrics(lang, synced, tracks, name, enhanced));
      }
    };
    emit_aux(media_library::LyricItemKind::Translation, "translation");
    emit_aux(media_library::LyricItemKind::Romanization, "pronunciation");
  }

  Json::Value lyrics_list;
  lyrics_list["structuredLyrics"] = std::move(structured);

  Json::Value payload;
  payload["lyricsList"] = std::move(lyrics_list);
  return ok(std::move(payload));
}

// ---------------------------------------------------------------------------
// 路由注册
// ---------------------------------------------------------------------------

// 注册一个方法的两个路径形态：/rest/<m> 与 /rest/<m>.view。
// handler 自己负责写访问日志（这样才能覆盖到「参数缺失」这类早退分支）。
void route(const char *method_name,
           std::function<http_server::ResponsePtr(
               const http_server::RequestPtr &)> handler) {
  const std::string base = std::string("/rest/") + method_name;
  const std::string dotted = base + ".view";
  const bool log = g_session.log_requests;

  auto wrap = [method_name, handler, log](const http_server::RequestPtr &req) {
    AccessLog access(req, method_name, log);
    const http_server::Query query(req);
    if (const std::optional<std::string> id = id_param(req); id.has_value()) {
      access.set_id(*id);
    }
    if (query.has("size") || query.has("offset")) {
      access.set_paging(subsonic::paging(req, 0).log_text());
    }
    http_server::ResponsePtr response = handler(req);
    // 失败也要在日志里体现：从已构造好的信封里取 code，不重复判一次业务。
    int code = 0;
    std::string name;
    const std::string body(response->body());
    if (body.find("\"status\":\"failed\"") != std::string::npos) {
      const std::size_t at = body.find("\"code\":");
      if (at != std::string::npos) {
        code = std::atoi(body.c_str() + at + 7);
      }
    }
    access.done(code, name);
    return response;
  };

  // Subsonic 客户端 GET/POST 都会用；HEAD 供 stream/coverArt 探测。
  for (const drogon::HttpMethod method :
       {drogon::Get, drogon::Post, drogon::Head}) {
    http_server::onPort(g_session.port, method, base, wrap);
    http_server::onPort(g_session.port, method, dotted, wrap);
  }

  // 预检必须挂在**精确路径**上：Drogon 的路由器先查精确路径表，命中后若没有
  // OPTIONS binder 就直接回 405，根本不会回落到 `/rest/.*` 的正则兜底。
  auto preflight = [](const http_server::RequestPtr &) {
    auto res = drogon::HttpResponse::newHttpResponse();
    res->setStatusCode(drogon::k204NoContent);
    return res;
  };
  http_server::onPort(g_session.port, drogon::Options, base, preflight);
  http_server::onPort(g_session.port, drogon::Options, dotted, preflight);
}

// 异步版本：handler 自己决定何时回调。访问日志挂在回调的最后一步，保证
// 「响应真正发出」之后才落行。
using AsyncHandler = std::function<void(
    const http_server::RequestPtr &,
    std::function<void(const http_server::ResponsePtr &)>)>;

void route_async(const char *method_name, AsyncHandler handler) {
  const std::string base = std::string("/rest/") + method_name;
  const std::string dotted = base + ".view";
  const bool log = g_session.log_requests;

  auto wrap = [method_name, handler, log](
                  const http_server::RequestPtr &req,
                  std::function<void(const http_server::ResponsePtr &)>
                      callback) {
    auto access = std::make_shared<AccessLog>(req, method_name, log);
    const http_server::Query query(req);
    if (const std::optional<std::string> id = id_param(req); id.has_value()) {
      access->set_id(*id);
    }
    if (query.has("size") || query.has("offset")) {
      access->set_paging(subsonic::paging(req, 0).log_text());
    }
    handler(req, [access, callback = std::move(callback)](
                     const http_server::ResponsePtr &response) {
      int code = 0;
      const std::string body(response->body());
      if (body.find("\"status\":\"failed\"") != std::string::npos) {
        const std::size_t at = body.find("\"code\":");
        if (at != std::string::npos) {
          code = std::atoi(body.c_str() + at + 7);
        }
      }
      access->done(code);
      callback(response);
    });
  };

  for (const drogon::HttpMethod method :
       {drogon::Get, drogon::Post, drogon::Head}) {
    http_server::onPortAsync(g_session.port, method, base, wrap);
    http_server::onPortAsync(g_session.port, method, dotted, wrap);
  }

  auto preflight = [](const http_server::RequestPtr &) {
    auto res = drogon::HttpResponse::newHttpResponse();
    res->setStatusCode(drogon::k204NoContent);
    return res;
  };
  http_server::onPort(g_session.port, drogon::Options, base, preflight);
  http_server::onPort(g_session.port, drogon::Options, dotted, preflight);
}

void register_routes() {
  route("ping", handle_ping);
  route("getLicense", handle_license);
  route("getOpenSubsonicExtensions", handle_open_subsonic_extensions);
  route("getMusicFolders", handle_music_folders);
  route("getUser", handle_user);
  route("getUsers", handle_users);
  route("getSong", handle_song);
  route("getAlbum", handle_album);
  route("getArtist", handle_artist);
  route("getAlbumInfo2", handle_album_info2);
  route("getArtistInfo2", handle_artist_info2);
  route_async("stream", handle_stream);
  route_async("download", handle_stream);
  route_async("getCoverArt", handle_cover_art);
  route("search2",
        [](const http_server::RequestPtr &req) { return handle_search(req, false); });
  route("search3",
        [](const http_server::RequestPtr &req) { return handle_search(req, true); });
  route("getPlaylists", handle_playlists);
  route("getPlaylist", handle_playlist);
  route("getLyricsBySongId", handle_lyrics_by_song_id);
}

} // namespace

void mount(const config::Config &cfg) {
  g_session.port = cfg.subsonic.port;
  g_session.server_name = cfg.subsonic.server_name;
  g_session.server_version = kServerVersion;
  g_session.log_requests = cfg.subsonic.log_requests;
  g_session.default_page_size = cfg.subsonic.default_page_size;
  g_session.cors_allow_origin = cfg.subsonic.cors_allow_origin;

  const int port = g_session.port;

  // Drogon 默认会在路由前自行短路 OPTIONS（只回 Allow 头），我们的 CORS 头就
  // 加不上去。给本端口 `/rest/*` 的预检打标记，让框架把请求交回路由；
  // 否则精确路径命中的预检会被路由器判成 405。
  drogon::app().registerPreRoutingAdvice(
      [port](const http_server::RequestPtr &req) {
        if (!req || req->method() != drogon::Options) {
          return;
        }
        if (static_cast<int>(req->localAddr().toPort()) != port) {
          return;
        }
        if (req->path().find("/rest/") == std::string::npos) {
          return;
        }
        req->attributes()->insert("drogon.customCORShandling", true);
      });

  // CORS 走 post-handling advice：只在本服务端口生效，原生客户端响应逐字节不变。
  drogon::app().registerPostHandlingAdvice(
      [port](const http_server::RequestPtr &req,
             const http_server::ResponsePtr &res) {
        if (!req || !res) {
          return;
        }
        if (static_cast<int>(req->localAddr().toPort()) != port) {
          return;
        }
        apply_cors(req, res);
      });

  // 预检：Drogon 不会自动短路 OPTIONS，这里为 /rest/* 显式注册一条。
  drogon::app().registerHandlerViaRegex(
      http_server::portPrefix(port) + "/rest/.*",
      [](const http_server::RequestPtr &,
         std::function<void(const http_server::ResponsePtr &)> &&cb) {
        auto res = drogon::HttpResponse::newHttpResponse();
        res->setStatusCode(drogon::k204NoContent);
        cb(res);
      },
      {drogon::internal::HttpConstraint{drogon::Options}});

  register_routes();

  std::cerr << "[subsonic] 生效端口 " << port
            << " server_name=" << cfg.subsonic.server_name
            << " page_size=" << cfg.subsonic.default_page_size
            << " log_requests=" << (cfg.subsonic.log_requests ? "on" : "off")
            << " cors="
            << (cfg.subsonic.cors_allow_origin.empty()
                    ? "off"
                    : cfg.subsonic.cors_allow_origin)
            << std::endl;

  // 框架日志降到 WARN：避免把整条 URL（含 u=/t=/s=/p= 凭据）打出来，
  // 排查全靠上面那行 [subsonic] 访问日志。
  http_server::setQuietLogging();
}

} // namespace subsonic
