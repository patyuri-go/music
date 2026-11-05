#include "api/subsonic/subsonic.h"

#include "api/subsonic/access_log.h"
#include "api/subsonic/dto.h"
#include "api/subsonic/envelope.h"
#include "api/subsonic/params.h"

#include "http_util.h"
#include "media_library/library_directory.h"
#include "media_library/media_runtime.h"
#include "media_library/source_ref.h"
#include "stream_http.h"

#include "crow/app.h"
#include "crow/http_request.h"
#include "crow/http_response.h"
#include "crow/json.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace subsonic {
namespace {

using media_library::Album;
using media_library::AlbumRef;
using media_library::Artist;
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
// 1) 走**全局中间件**而不是各 handler 自己加头。Crow 1.3.1 在 routing.h 里对
//    OPTIONS 做了内建短路（直接 204 + Allow），请求根本到不了路由；但连接层
//    complete_request() 在短路时仍会调用全局中间件的 after_handle，所以只有
//    中间件这一层能同时覆盖「正常响应」和「预检响应」。
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

void apply_cors(const crow::request &req, crow::response &res) {
  const std::string &configured = g_session.cors_allow_origin;
  if (configured.empty()) {
    return;  // 显式关闭。
  }

  if (req.method == crow::HTTPMethod::Options) {
    // 预检走的是 Crow 的路由短路（http_connection::handle_url），发生在**请求头
    // 解析之前**，所以这里拿不到 Origin，只能回 `*`。不是敷衍：真正的响应仍然
    // 只对白名单来源回显 Origin，浏览器照样会拦掉不在名单里的页面。
    // 白名单模式下预检无法按来源区分，宁可失败也不 fail-open。
    if (configured != "*") {
      return;
    }
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS, HEAD");
    // 预检拿不到 Access-Control-Request-Headers，直接放行任意自定义头。
    // 安全性不靠这里：凭据在 query string，且响应侧仍按 Origin 白名单收口。
    res.set_header("Access-Control-Allow-Headers", "*");
    res.set_header("Access-Control-Max-Age", "86400");
    return;
  }

  const std::string origin = req.get_header_value("Origin");
  if (origin.empty()) {
    return;  // 原生客户端：响应逐字节不变。
  }
  const std::string echoed = cors_origin_of(origin);
  if (echoed.empty()) {
    return;
  }
  res.set_header("Access-Control-Allow-Origin", echoed);
  res.set_header("Vary", "Origin");
  // 浏览器默认读不到这些头；Web 播放器靠 Content-Range 算时长和拖动进度。
  res.set_header("Access-Control-Expose-Headers",
                 "Content-Length, Content-Range, Accept-Ranges, Content-Type");
}

struct CorsMiddleware {
  struct context {};
  void before_handle(crow::request & /*req*/, crow::response & /*res*/,
                     context & /*ctx*/) {}
  void after_handle(crow::request &req, crow::response &res, context & /*ctx*/) {
    apply_cors(req, res);
  }
};

using App = crow::Crow<CorsMiddleware>;

// 构造一条成功响应。
crow::response ok(crow::json::wvalue payload) {
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

crow::response handle_ping(const crow::request &) { return ok(crow::json::wvalue::object()); }

crow::response handle_license(const crow::request &) {
  crow::json::wvalue payload;
  crow::json::wvalue license;
  license["valid"] = true;
  license["email"] = "anonymous@localhost";
  license["licenseExpires"] = "2099-12-31T23:59:59";
  payload["license"] = std::move(license);
  return ok(std::move(payload));
}

// OpenSubsonic 扩展声明：客户端（Feishin 等）靠这个判断服务端是否支持
// getLyricsBySongId 的结构化歌词及 v2 的 enhanced 字段。只宣告真正实现的扩展。
crow::response handle_open_subsonic_extensions(const crow::request &) {
  crow::json::wvalue entry;
  entry["name"] = "songLyrics";
  entry["versions"] = crow::json::wvalue::list{1, 2};
  crow::json::wvalue::list extensions;
  extensions.push_back(std::move(entry));
  crow::json::wvalue payload;
  payload["openSubsonicExtensions"] =
      crow::json::wvalue(std::move(extensions));
  return ok(std::move(payload));
}

crow::response handle_music_folders(const crow::request &) {
  const std::shared_ptr<media_library::LibraryRegistry> registry =
      media_library::library_registry_ptr();
  crow::json::wvalue::list folders;
  if (registry != nullptr) {
    media_library::LibraryDirectory directory(registry);
    for (const media_library::LibraryView &view : directory.list()) {
      if (!view.enabled) {
        continue;
      }
      folders.push_back(music_folder_dto(view.library_id, view.display_name));
    }
  }
  crow::json::wvalue payload;
  payload["musicFolders"] = crow::json::wvalue::object();
  payload["musicFolders"]["musicFolder"] = crow::json::wvalue(std::move(folders));
  return ok(std::move(payload));
}

// ---------------------------------------------------------------------------
// 账号
// ---------------------------------------------------------------------------

// 本服务不校验账号：只回一个合成用户，用户名取客户端传来的值。
// getUsers 没有 username 参数，退回凭据参数 u；两者都空时用 "admin"。
crow::response handle_user(const crow::request &req) {
  crow::json::wvalue payload;
  payload["user"] = user_dto(text(req, "username"));
  return ok(std::move(payload));
}

crow::response handle_users(const crow::request &req) {
  crow::json::wvalue payload;
  crow::json::wvalue::list users;
  users.push_back(user_dto(text(req, "u")));
  payload["users"] = crow::json::wvalue::object();
  payload["users"]["user"] = crow::json::wvalue(std::move(users));
  return ok(std::move(payload));
}

// ---------------------------------------------------------------------------
// 单实体
// ---------------------------------------------------------------------------

crow::response handle_song(const crow::request &req) {
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
  crow::json::wvalue payload;
  payload["song"] = song_dto(result.value().value);
  return ok(std::move(payload));
}

crow::response handle_album(const crow::request &req) {
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

  crow::json::wvalue node = album_dto(album.value().value, true);

  // song[] 与 songCount / duration 都来自同一批 list_album_tracks。
  // 本地库给大 limit 会一次返回全部；网易云若还有 next_cursor 就继续跟。
  PageRequest page;
  page.limit = kFetchAllPageSize;
  crow::json::wvalue::list songs;
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
      songs.push_back(song_dto(item));
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
  node["song"] = crow::json::wvalue(std::move(songs));

  crow::json::wvalue payload;
  payload["album"] = std::move(node);
  return ok(std::move(payload));
}

crow::response handle_artist(const crow::request &req) {
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
  crow::json::wvalue::list album_list;
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
      album_list.push_back(album_dto(item, false));
    }
    if (!value.has_more || !value.next_cursor.has_value()) {
      break;
    }
    if (cursor.has_value() && *value.next_cursor == *cursor) {
      break;
    }
    cursor = value.next_cursor;
  }

  crow::json::wvalue node =
      artist_dto(artist.value().value, static_cast<int>(album_list.size()));
  node["album"] = crow::json::wvalue(std::move(album_list));

  crow::json::wvalue payload;
  payload["artist"] = std::move(node);
  return ok(std::move(payload));
}

// ---------------------------------------------------------------------------
// 字节出口
// ---------------------------------------------------------------------------

crow::response handle_stream(const crow::request &req) {
  const std::optional<std::string> raw = id_param(req);
  if (!raw.has_value()) {
    return failure_response(Code::MissingParameter, "id is required");
  }
  const std::optional<SourceRef> ref = parse_id(*raw, EntityType::Track);
  if (!ref.has_value()) {
    return failure_response(Code::NotFound, "song not found: " + *raw);
  }
  // 音频字节统一从播放会话出：选源交给 MediaRouter，locator 不外泄。
  return stream_http::serve_track(
      req, track_ref_of(*ref),
      req.method == crow::HTTPMethod::HEAD);
}

crow::response handle_cover_art(const crow::request &req) {
  const std::optional<std::string> raw = id_param(req);
  if (!raw.has_value()) {
    return failure_response(Code::MissingParameter, "id is required");
  }
  const std::optional<SourceRef> ref = media_library::parse_ref(*raw);
  if (!ref.has_value() ||
      (ref->entity_type != EntityType::Track &&
       ref->entity_type != EntityType::Album &&
       ref->entity_type != EntityType::Artist &&
       ref->entity_type != EntityType::AlbumArtist)) {
    return failure_response(Code::NotFound, "cover art not found");
  }
  media_library::ArtworkRef artwork;
  artwork.owner = ref->entity_type == EntityType::AlbumArtist
                      ? media_library::as_artist_ref(*ref)
                      : *ref;
  return stream_http::serve_artwork(req, artwork,
                                    req.method == crow::HTTPMethod::HEAD);
}

// ---------------------------------------------------------------------------
// 搜索
// ---------------------------------------------------------------------------

media_library::EntityType entity_type_of(const char *text) {
  return media_library::parse_entity_type(text)
      .value_or(EntityType::Track);
}

// search2 / search3 共用：差别只在输出结构的键名。
crow::response handle_search(const crow::request &req, bool id3) {
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
                               crow::json::wvalue::list &out)
      -> std::optional<crow::response> {
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
      out.push_back(search_hit_dto(item.value));
    }
    return std::nullopt;
  };

  crow::json::wvalue::list songs;
  crow::json::wvalue::list albums;
  crow::json::wvalue::list artists;
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
  crow::json::wvalue result_node;
  result_node["song"] = crow::json::wvalue(std::move(songs));
  result_node[kind_album] = crow::json::wvalue(std::move(albums));
  result_node[kind_artist] = crow::json::wvalue(std::move(artists));

  crow::json::wvalue payload;
  payload[id3 ? "searchResult3" : "searchResult2"] = std::move(result_node);
  return ok(std::move(payload));
}

// ---------------------------------------------------------------------------
// 歌单
// ---------------------------------------------------------------------------

crow::response handle_playlists(const crow::request &req) {
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
  crow::json::wvalue::list items;
  for (const auto &item : result.value().items) {
    items.push_back(playlist_dto(item.value, false));
  }
  crow::json::wvalue payload;
  payload["playlists"] = crow::json::wvalue::object();
  payload["playlists"]["playlist"] = crow::json::wvalue(std::move(items));
  return ok(std::move(payload));
}

crow::response handle_playlist(const crow::request &req) {
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
  crow::json::wvalue payload;
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
  const std::vector<media_library::LyricWord> *words = nullptr;
};

// Subsonic 的 cue 用 **UTF-8 字节偏移** 而且是闭区间；跨行拼字时必须按字节累加，
// 不能按码点数（中日常用字都是 3 字节，混排时按码点算一定错位）。
crow::json::wvalue::list cue_list_of(const std::vector<media_library::LyricWord> &words,
                                     std::optional<std::int64_t> line_end_ms) {
  crow::json::wvalue::list cues;

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

    crow::json::wvalue cue;
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
    cues.push_back(std::move(cue));
  }
  return cues;
}

// 组装一个 structuredLyrics 元素。
// kind 只在 enhanced 时输出：main / translation / pronunciation。
crow::json::wvalue structured_lyrics(const std::string &lang, bool synced,
                                     const std::vector<LyricTrack> &tracks,
                                     const std::string &kind, bool enhanced) {
  crow::json::wvalue::list lines;
  crow::json::wvalue::list cue_lines;

  for (std::size_t index = 0; index < tracks.size(); ++index) {
    const LyricTrack &track = tracks[index];

    crow::json::wvalue line;
    line["value"] = track.text;
    if (synced && track.start_ms.has_value()) {
      line["start"] = *track.start_ms;
    }
    lines.push_back(std::move(line));

    // cueLine 只在该轨真的有逐字时间、且整体是同步歌词时才给。
    if (!enhanced || !synced || track.words == nullptr || track.words->empty()) {
      continue;
    }
    std::string value;
    for (const auto &word : *track.words) {
      value += word.text;
    }
    crow::json::wvalue cue_line;
    cue_line["index"] = static_cast<int>(index);
    if (track.start_ms.has_value()) {
      cue_line["start"] = *track.start_ms;
    }
    if (track.end_ms.has_value()) {
      cue_line["end"] = *track.end_ms;
    }
    cue_line["value"] = value;
    cue_line["cue"] = crow::json::wvalue(cue_list_of(*track.words, track.end_ms));
    cue_lines.push_back(std::move(cue_line));
  }

  crow::json::wvalue node;
  node["lang"] = lang;
  node["synced"] = synced;
  node["line"] = crow::json::wvalue(std::move(lines));
  if (!cue_lines.empty()) {
    node["cueLine"] = crow::json::wvalue(std::move(cue_lines));
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

crow::response handle_lyrics_by_song_id(const crow::request &req) {
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

  const bool synced = lyrics.mode != media_library::LyricsMode::Unsynced;
  const std::string lang = lyric_lang(lyrics);

  std::vector<LyricTrack> main_tracks;
  main_tracks.reserve(lyrics.lines.size());
  for (const auto &line : lyrics.lines) {
    LyricTrack track;
    track.text = line.text;
    track.start_ms = line.start_ms;
    track.end_ms = line.end_ms;
    track.words = &line.words;
    main_tracks.push_back(std::move(track));
  }

  crow::json::wvalue::list structured;
  structured.push_back(
      structured_lyrics(lang, synced, main_tracks, "main", enhanced));

  if (enhanced) {
    // 译文与注音各自是一条独立轨（kind 区分），行时间与正文行对齐。
    const auto emit_aux = [&](media_library::LyricAuxKind kind,
                              const char *name) {
      std::vector<LyricTrack> tracks;
      tracks.reserve(lyrics.lines.size());
      bool any = false;
      for (const auto &line : lyrics.lines) {
        const media_library::LyricAux *found = nullptr;
        for (const auto &aux : line.aux) {
          if (aux.kind == kind && !aux.text.empty()) {
            found = &aux;
            break;
          }
        }
        if (found == nullptr) {
          // OpenSubsonic：副轨与正文轨**不要求 1:1 对齐**，缺译文的行直接不出，
          // 不要用空串占位——客户端会把空行渲染成空白字幕。
          continue;
        }
        LyricTrack track;
        track.text = found->text;
        track.start_ms = line.start_ms;
        track.end_ms = line.end_ms;
        track.words = &found->words;
        any = true;
        tracks.push_back(std::move(track));
      }
      if (any) {
        structured.push_back(
            structured_lyrics(lang, synced, tracks, name, enhanced));
      }
    };
    emit_aux(media_library::LyricAuxKind::Translation, "translation");
    emit_aux(media_library::LyricAuxKind::Romanization, "pronunciation");
  }

  crow::json::wvalue lyrics_list;
  lyrics_list["structuredLyrics"] = crow::json::wvalue(std::move(structured));

  crow::json::wvalue payload;
  payload["lyricsList"] = std::move(lyrics_list);
  return ok(std::move(payload));
}

// ---------------------------------------------------------------------------
// 路由注册
// ---------------------------------------------------------------------------

// 注册一个方法的两个路径形态：/rest/<m> 与 /rest/<m>.view。
// handler 自己负责写访问日志（这样才能覆盖到「参数缺失」这类早退分支）。
void route(App &app, const char *method_name,
           std::function<crow::response(const crow::request &)> handler) {
  const std::string base = std::string("/rest/") + method_name;
  const std::string dotted = base + ".view";
  const bool log = g_session.log_requests;

  auto wrap = [method_name, handler, log](const crow::request &req) {
    AccessLog access(req, method_name, log);
    if (const std::optional<std::string> id = id_param(req); id.has_value()) {
      access.set_id(*id);
    }
    if (req.url_params.get("size") != nullptr ||
        req.url_params.get("offset") != nullptr) {
      access.set_paging(subsonic::paging(req, 0).log_text());
    }
    crow::response response = handler(req);
    // 失败也要在日志里体现：从已构造好的信封里取 code，不重复判一次业务。
    int code = 0;
    std::string name;
    if (response.body.find("\"status\":\"failed\"") != std::string::npos) {
      const std::size_t at = response.body.find("\"code\":");
      if (at != std::string::npos) {
        code = std::atoi(response.body.c_str() + at + 7);
      }
    }
    access.done(code, name);
    return response;
  };

  app.route_dynamic(base)(wrap);
  app.route_dynamic(dotted)(wrap);
}

void register_routes(App &app) {
  route(app, "ping", handle_ping);
  route(app, "getLicense", handle_license);
  route(app, "getOpenSubsonicExtensions", handle_open_subsonic_extensions);
  route(app, "getMusicFolders", handle_music_folders);
  route(app, "getUser", handle_user);
  route(app, "getUsers", handle_users);
  route(app, "getSong", handle_song);
  route(app, "getAlbum", handle_album);
  route(app, "getArtist", handle_artist);
  route(app, "stream", handle_stream);
  route(app, "download", handle_stream);
  route(app, "getCoverArt", handle_cover_art);
  route(app, "search2",
        [](const crow::request &req) { return handle_search(req, false); });
  route(app, "search3",
        [](const crow::request &req) { return handle_search(req, true); });
  route(app, "getPlaylists", handle_playlists);
  route(app, "getPlaylist", handle_playlist);
  route(app, "getLyricsBySongId", handle_lyrics_by_song_id);
}

} // namespace

void run(const config::Config &cfg) {
  g_session.server_name = cfg.subsonic.server_name;
  g_session.server_version = kServerVersion;
  g_session.log_requests = cfg.subsonic.log_requests;
  g_session.default_page_size = cfg.subsonic.default_page_size;
  g_session.cors_allow_origin = cfg.subsonic.cors_allow_origin;

  App app;
  register_routes(app);

  std::cerr << "[subsonic] 生效端口 " << cfg.subsonic.port
            << " server_name=" << cfg.subsonic.server_name
            << " page_size=" << cfg.subsonic.default_page_size
            << " log_requests=" << (cfg.subsonic.log_requests ? "on" : "off")
            << " cors="
            << (cfg.subsonic.cors_allow_origin.empty()
                    ? "off"
                    : cfg.subsonic.cors_allow_origin)
            << std::endl;

  // Crow 自己的 INFO 日志会把整条 URL（含 u=/t=/s=/p= 凭据）打出来，
  // 这里降到 WARNING：排查全靠上面那行 [subsonic] 访问日志。
  app.loglevel(crow::LogLevel::Warning);
  app.port(cfg.subsonic.port).multithreaded().concurrency(8).run();
}

} // namespace subsonic
