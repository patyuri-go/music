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

// 一次请求的公共上下文：服务器名与是否记日志。
struct Session {
  std::string server_name;
  std::string server_version;
  bool log_requests = true;
  int default_page_size = 50;
};

Session g_session;

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

  // song[] 与 songCount / duration 都来自同一次 list_album_tracks。
  PageRequest page;
  page.limit = 0;
  media_library::Result<media_library::Sourced<media_library::Page<Track>>> tracks =
      router->list_album_tracks(album_ref, page, CallContext{});
  crow::json::wvalue::list songs;
  long long total_ms = 0;
  if (tracks.ok()) {
    for (const auto &item : tracks.value().value.items) {
      total_ms += item.duration_ms.value_or(0);
      songs.push_back(song_dto(item));
    }
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
  page.limit = 0;
  media_library::Result<media_library::Sourced<media_library::Page<Album>>> albums =
      router->list_artist_albums(artist_ref, page, CallContext{});
  crow::json::wvalue::list album_list;
  if (albums.ok()) {
    for (const auto &item : albums.value().value.items) {
      album_list.push_back(album_dto(item, false));
    }
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

  media_library::SearchQuery query;
  query.text = text(req, "query");
  if (query.text.empty()) {
    return failure_response(Code::MissingParameter, "query is required");
  }

  const int song_count = integer(req, "songCount", 0);
  const int album_count = integer(req, "albumCount", 0);
  const int artist_count = integer(req, "artistCount", 0);

  // 三类都要，才能在聚合结果里按类型各取所需；条数在下面各自截断。
  query.types.push_back(EntityType::Track);
  query.types.push_back(EntityType::Album);
  query.types.push_back(EntityType::Artist);

  const int wanted = song_count + album_count + artist_count;
  query.page.limit = wanted > 0 ? wanted : g_session.default_page_size;
  if (query.page.limit < 0) {
    query.page.limit = g_session.default_page_size;
  }

  // musicFolderId：按 ref.library_id 精确过滤（参数名大小写不敏感）。
  const std::string folder = text(req, "musicFolderId");

  media_library::Result<media_library::RoutedPage<media_library::SearchHit>>
      result = router->search(query, CallContext{});
  if (!result.ok()) {
    return failure_response(code_of(result.error().code),
                            error_name(result.error().code));
  }

  // 路由器已按 library_priority 分层排好序；这里只按类型分桶并截断，
  // 绝不重排——否则远端条目会插进本地组里。
  crow::json::wvalue::list songs;
  crow::json::wvalue::list albums;
  crow::json::wvalue::list artists;
  const int song_limit = song_count > 0 ? song_count : g_session.default_page_size;
  const int album_limit =
      album_count > 0 ? album_count : g_session.default_page_size;
  const int artist_limit =
      artist_count > 0 ? artist_count : g_session.default_page_size;

  for (const auto &item : result.value().items) {
    const media_library::SearchHit &hit = item.value;
    if (!folder.empty() && hit.ref.library_id != folder) {
      continue;
    }
    switch (hit.kind) {
    case EntityType::Track:
      if (static_cast<int>(songs.size()) < song_limit) {
        songs.push_back(search_hit_dto(hit));
      }
      break;
    case EntityType::Album:
      if (static_cast<int>(albums.size()) < album_limit) {
        albums.push_back(search_hit_dto(hit));
      }
      break;
    case EntityType::Artist:
    case EntityType::AlbumArtist:
      if (static_cast<int>(artists.size()) < artist_limit) {
        artists.push_back(search_hit_dto(hit));
      }
      break;
    default:
      break;
    }
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

  crow::json::wvalue::list lines;
  for (const auto &line : lyrics.lines) {
    crow::json::wvalue entry;
    entry["value"] = line.text;
    if (line.start_ms.has_value()) {
      entry["start"] = *line.start_ms;
    }
    lines.push_back(std::move(entry));
  }

  crow::json::wvalue lyrics_node;
  lyrics_node["id"] = *raw;
  lyrics_node["line"] = crow::json::wvalue(std::move(lines));
  lyrics_node["synced"] = lyrics.mode != media_library::LyricsMode::Unsynced;

  crow::json::wvalue::list structured;
  structured.push_back(std::move(lyrics_node));

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
void route(crow::SimpleApp &app, const char *method_name,
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

void register_routes(crow::SimpleApp &app) {
  route(app, "ping", handle_ping);
  route(app, "getLicense", handle_license);
  route(app, "getMusicFolders", handle_music_folders);
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

  crow::SimpleApp app;
  register_routes(app);

  std::cerr << "[subsonic] 生效端口 " << cfg.subsonic.port
            << " server_name=" << cfg.subsonic.server_name
            << " page_size=" << cfg.subsonic.default_page_size
            << " log_requests=" << (cfg.subsonic.log_requests ? "on" : "off")
            << std::endl;

  // Crow 自己的 INFO 日志会把整条 URL（含 u=/t=/s=/p= 凭据）打出来，
  // 这里降到 WARNING：排查全靠上面那行 [subsonic] 访问日志。
  app.loglevel(crow::LogLevel::Warning);
  app.port(cfg.subsonic.port).multithreaded().concurrency(8).run();
}

} // namespace subsonic
