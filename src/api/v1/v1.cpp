#include "api/v1/v1.h"

#include "crow/json.h"
#include "http_util.h"
#include "media_library/library_directory.h"
#include "media_library/media_runtime.h"
#include "stream_http.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace api_v1 {
namespace {

using namespace media_library;

// ---------------------------------------------------------------------------
// 错误到 HTTP 的映射由 stream_http 统一提供：状态码与响应体全进程只有一份。
// ---------------------------------------------------------------------------

crow::response fail(const Error &error) {
  return stream_http::error_response(error);
}

crow::response invalid(const std::string &message) {
  return fail(Error::make(ErrorCode::InvalidRequest, message));
}

crow::response unavailable() {
  return fail(Error::make(ErrorCode::ProviderUnavailable,
                          "media runtime is not initialized"));
}

// ---------------------------------------------------------------------------
// 领域模型序列化
// ---------------------------------------------------------------------------

crow::json::wvalue ref_json(const SourceRef &ref) {
  crow::json::wvalue out;
  out["ref"] = to_string(ref);
  out["library_id"] = ref.library_id;
  out["type"] = std::string(entity_type_name(ref.entity_type));
  out["id"] = ref.entity_id;
  return out;
}

crow::json::wvalue restrictions_json(const std::vector<Restriction> &items) {
  crow::json::wvalue::list out;
  out.reserve(items.size());
  for (const Restriction &item : items) {
    crow::json::wvalue entry;
    entry["code"] = std::string(restriction_code_name(item.code));
    entry["message"] = item.message;
    out.push_back(std::move(entry));
  }
  return crow::json::wvalue(std::move(out));
}

crow::json::wvalue artwork_json(const ArtworkRef &artwork) {
  crow::json::wvalue out;
  out["owner"] = ref_json(artwork.owner);
  out["kind"] = artwork.kind;
  if (artwork.artwork_id.has_value()) {
    out["artwork_id"] = *artwork.artwork_id;
  } else {
    out["artwork_id"] = nullptr;
  }
  out["cache_tag"] = artwork.cache_tag;
  return out;
}

crow::json::wvalue artist_links_json(const std::vector<ArtistLink> &items) {
  crow::json::wvalue::list out;
  out.reserve(items.size());
  for (const ArtistLink &item : items) {
    crow::json::wvalue entry;
    entry["name"] = item.name;
    entry["artist"] = ref_json(item.ref);
    out.push_back(std::move(entry));
  }
  return crow::json::wvalue(std::move(out));
}

crow::json::wvalue album_artist_links_json(
    const std::vector<AlbumArtistLink> &items) {
  crow::json::wvalue::list out;
  out.reserve(items.size());
  for (const AlbumArtistLink &item : items) {
    crow::json::wvalue entry;
    entry["name"] = item.name;
    entry["album_artist"] = ref_json(item.ref);
    out.push_back(std::move(entry));
  }
  return crow::json::wvalue(std::move(out));
}

crow::json::wvalue track_json(const Track &track) {
  crow::json::wvalue out;
  out["ref"] = ref_json(track.ref);
  out["title"] = track.title;
  out["artists"] = artist_links_json(track.artists);
  out["album_artists"] = album_artist_links_json(track.album_artists);
  if (track.album.has_value()) {
    crow::json::wvalue album;
    album["title"] = track.album->title;
    album["album"] = ref_json(track.album->ref);
    out["album"] = std::move(album);
  } else {
    out["album"] = nullptr;
  }
  out["date"] = track.date.has_value() ? crow::json::wvalue(*track.date)
                                       : crow::json::wvalue(nullptr);
  out["duration_ms"] = track.duration_ms.has_value()
                           ? crow::json::wvalue(*track.duration_ms)
                           : crow::json::wvalue(nullptr);
  out["track_no"] = track.track_no.has_value()
                        ? crow::json::wvalue(*track.track_no)
                        : crow::json::wvalue(nullptr);
  out["disc_no"] = track.disc_no.has_value()
                       ? crow::json::wvalue(*track.disc_no)
                       : crow::json::wvalue(nullptr);
  out["year"] = track.year.has_value() ? crow::json::wvalue(*track.year)
                                       : crow::json::wvalue(nullptr);

  crow::json::wvalue::list genres;
  genres.reserve(track.genres.size());
  for (const std::string &genre : track.genres) {
    genres.push_back(genre);
  }
  out["genres"] = std::move(genres);

  out["artwork"] = track.artwork.has_value()
                       ? artwork_json(*track.artwork)
                       : crow::json::wvalue(nullptr);
  out["lyrics_available"] = track.lyrics_available;
  out["playable"] = track.playable;
  out["restrictions"] = restrictions_json(track.restrictions);

  crow::json::wvalue audio;
  audio["bitrate_kbps"] = track.audio.bitrate_kbps.has_value()
                              ? crow::json::wvalue(*track.audio.bitrate_kbps)
                              : crow::json::wvalue(nullptr);
  audio["sample_rate"] = track.audio.sample_rate.has_value()
                             ? crow::json::wvalue(*track.audio.sample_rate)
                             : crow::json::wvalue(nullptr);
  audio["channels"] = track.audio.channels.has_value()
                          ? crow::json::wvalue(*track.audio.channels)
                          : crow::json::wvalue(nullptr);
  audio["format"] = track.audio.format;
  audio["size_bytes"] = track.audio.size_bytes.has_value()
                            ? crow::json::wvalue(*track.audio.size_bytes)
                            : crow::json::wvalue(nullptr);
  out["audio"] = std::move(audio);
  return out;
}

crow::json::wvalue album_json(const Album &album) {
  crow::json::wvalue out;
  out["ref"] = ref_json(album.ref);
  out["title"] = album.title;
  out["artists"] = album_artist_links_json(album.artists);
  out["date"] = album.date.has_value() ? crow::json::wvalue(*album.date)
                                       : crow::json::wvalue(nullptr);
  out["year"] = album.year.has_value() ? crow::json::wvalue(*album.year)
                                       : crow::json::wvalue(nullptr);
  crow::json::wvalue::list genres;
  genres.reserve(album.genres.size());
  for (const std::string &genre : album.genres) {
    genres.push_back(genre);
  }
  out["genres"] = std::move(genres);
  out["artwork"] = album.artwork.has_value()
                       ? artwork_json(*album.artwork)
                       : crow::json::wvalue(nullptr);
  return out;
}

crow::json::wvalue artist_json(const Artist &artist) {
  crow::json::wvalue out;
  out["ref"] = ref_json(artist.ref);
  out["name"] = artist.name;
  out["sort_name"] = artist.sort_name;
  out["artwork"] = artist.artwork.has_value()
                       ? artwork_json(*artist.artwork)
                       : crow::json::wvalue(nullptr);
  return out;
}

crow::json::wvalue playlist_json(const Playlist &playlist) {
  crow::json::wvalue out;
  out["ref"] = ref_json(playlist.ref);
  out["name"] = playlist.name;
  out["description"] = playlist.description;
  crow::json::wvalue::list tracks;
  tracks.reserve(playlist.tracks.size());
  for (const TrackRef &track : playlist.tracks) {
    tracks.push_back(to_string(track));
  }
  out["tracks"] = std::move(tracks);
  return out;
}

crow::json::wvalue lyrics_json(const Lyrics &lyrics) {
  crow::json::wvalue out;
  out["track"] = ref_json(lyrics.track);
  out["language"] = lyrics.language.has_value()
                        ? crow::json::wvalue(*lyrics.language)
                        : crow::json::wvalue(nullptr);
  switch (lyrics.mode) {
  case LyricsMode::WordSynced:
    out["mode"] = "word_synced";
    break;
  case LyricsMode::LineSynced:
    out["mode"] = "line_synced";
    break;
  case LyricsMode::Unsynced:
    out["mode"] = "unsynced";
    break;
  }
  crow::json::wvalue::list lines;
  lines.reserve(lyrics.lines.size());
  for (const LyricLine &line : lyrics.lines) {
    crow::json::wvalue entry;
    entry["start_ms"] = line.start_ms.has_value()
                            ? crow::json::wvalue(*line.start_ms)
                            : crow::json::wvalue(nullptr);
    entry["end_ms"] = line.end_ms.has_value() ? crow::json::wvalue(*line.end_ms)
                                              : crow::json::wvalue(nullptr);
    entry["text"] = line.text;
    crow::json::wvalue::list words;
    words.reserve(line.words.size());
    for (const LyricWord &word : line.words) {
      crow::json::wvalue item;
      item["start_ms"] = word.start_ms;
      item["end_ms"] = word.end_ms.has_value()
                           ? crow::json::wvalue(*word.end_ms)
                           : crow::json::wvalue(nullptr);
      item["text"] = word.text;
      words.push_back(std::move(item));
    }
    entry["words"] = std::move(words);
    lines.push_back(std::move(entry));
  }
  out["lines"] = std::move(lines);
  out["plain_text"] = lyrics.plain_text.has_value()
                          ? crow::json::wvalue(*lyrics.plain_text)
                          : crow::json::wvalue(nullptr);
  return out;
}

// internal_locator 是进程内定位符，绝不写进响应（5.4.4）。
crow::json::wvalue stream_json(const PlaybackSource &source) {
  crow::json::wvalue out;
  out["track"] = ref_json(source.track);
  switch (source.kind) {
  case PlaybackKind::LocalFile:
    out["kind"] = "local_file";
    break;
  case PlaybackKind::WebDav:
    out["kind"] = "webdav";
    break;
  case PlaybackKind::RemoteStream:
    out["kind"] = "remote_stream";
    break;
  case PlaybackKind::Cache:
    out["kind"] = "cache";
    break;
  }
  out["quality"] = source.quality.has_value()
                       ? crow::json::wvalue(*source.quality)
                       : crow::json::wvalue(nullptr);
  out["bitrate_kbps"] = source.bitrate_kbps.has_value()
                            ? crow::json::wvalue(*source.bitrate_kbps)
                            : crow::json::wvalue(nullptr);
  out["mime_type"] = source.mime_type;
  out["content_length"] = source.content_length.has_value()
                              ? crow::json::wvalue(*source.content_length)
                              : crow::json::wvalue(nullptr);
  out["range_supported"] = source.range_supported;
  out["expires_at"] = source.expires_at.has_value()
                          ? crow::json::wvalue(*source.expires_at)
                          : crow::json::wvalue(nullptr);
  out["access_mode"] =
      source.access_mode == AccessMode::Redirect ? "redirect" : "proxy";
  out["cacheable"] = source.cacheable;
  out["restrictions"] = restrictions_json(source.restrictions);
  return out;
}

crow::json::wvalue hit_json(const SearchHit &hit) {
  crow::json::wvalue out;
  out["kind"] = std::string(entity_type_name(hit.kind));
  out["ref"] = ref_json(hit.ref);
  out["title"] = hit.title;
  out["subtitle"] = hit.subtitle.has_value()
                        ? crow::json::wvalue(*hit.subtitle)
                        : crow::json::wvalue(nullptr);
  out["artists"] = artist_links_json(hit.artists);
  out["album_artists"] = album_artist_links_json(hit.album_artists);
  out["artwork"] = hit.artwork.has_value() ? artwork_json(*hit.artwork)
                                           : crow::json::wvalue(nullptr);
  out["duration_ms"] = hit.duration_ms.has_value()
                           ? crow::json::wvalue(*hit.duration_ms)
                           : crow::json::wvalue(nullptr);
  out["playable"] = hit.playable;
  return out;
}

// ---------------------------------------------------------------------------
// 信封序列化
// ---------------------------------------------------------------------------

crow::json::wvalue origin_json(const Origin &origin) {
  crow::json::wvalue out;
  out["library_id"] = origin.library_id;
  out["ref"] = to_string(origin.ref);
  if (origin.aspect.has_value()) {
    out["aspect"] = std::string(aspect_name(*origin.aspect));
  } else {
    out["aspect"] = nullptr;
  }
  out["substituted"] = origin.substituted;
  crow::json::wvalue::list chain;
  chain.reserve(origin.chain.size());
  for (const RouteAttempt &attempt : origin.chain) {
    crow::json::wvalue hop;
    hop["library_id"] = attempt.library_id;
    hop["ok"] = attempt.ok;
    if (attempt.error.has_value()) {
      hop["error"] = std::string(error_code_name(*attempt.error));
    } else {
      hop["error"] = nullptr;
    }
    hop["elapsed_ms"] = attempt.elapsed_ms;
    chain.push_back(std::move(hop));
  }
  out["chain"] = std::move(chain);
  return out;
}

crow::json::wvalue sources_json(const std::vector<SourceStatus> &sources) {
  crow::json::wvalue::list out;
  out.reserve(sources.size());
  for (const SourceStatus &status : sources) {
    crow::json::wvalue entry;
    entry["library_id"] = status.library_id;
    entry["state"] = std::string(source_state_name(status.state));
    if (status.error.has_value()) {
      entry["error"] = std::string(error_code_name(status.error->code));
    } else {
      entry["error"] = nullptr;
    }
    entry["item_count"] = status.item_count;
    entry["elapsed_ms"] = status.elapsed_ms;
    out.push_back(std::move(entry));
  }
  return crow::json::wvalue(std::move(out));
}

template <typename T, typename Fn>
crow::json::wvalue sourced_json(const Sourced<T> &sourced, Fn fn) {
  crow::json::wvalue out;
  out["value"] = fn(sourced.value);
  out["origin"] = origin_json(sourced.origin);
  return out;
}

template <typename T, typename Fn>
crow::json::wvalue sourced_page_json(const Sourced<Page<T>> &sourced, Fn fn) {
  crow::json::wvalue page;
  crow::json::wvalue::list items;
  items.reserve(sourced.value.items.size());
  for (const T &item : sourced.value.items) {
    items.push_back(fn(item));
  }
  page["items"] = std::move(items);
  page["next_cursor"] = sourced.value.next_cursor.has_value()
                            ? crow::json::wvalue(*sourced.value.next_cursor)
                            : crow::json::wvalue(nullptr);
  page["has_more"] = sourced.value.has_more;

  crow::json::wvalue out;
  out["value"] = std::move(page);
  out["origin"] = origin_json(sourced.origin);
  return out;
}

template <typename T, typename Fn>
crow::json::wvalue routed_page_json(const RoutedPage<T> &page, Fn fn) {
  crow::json::wvalue out;
  crow::json::wvalue::list items;
  items.reserve(page.items.size());
  for (const Sourced<T> &item : page.items) {
    items.push_back(sourced_json(item, fn));
  }
  out["items"] = std::move(items);
  out["next_cursor"] = page.next_cursor.has_value()
                           ? crow::json::wvalue(*page.next_cursor)
                           : crow::json::wvalue(nullptr);
  out["has_more"] = page.has_more;
  out["sources"] = sources_json(page.sources);
  return out;
}

// 7.1 会话序列化：这里没有 locator，也没有 internal_locator 的任何形态。
crow::json::wvalue session_json(const PlaybackSession &session) {
  crow::json::wvalue summary;
  summary["track"] = ref_json(session.stream.value.track);
  summary["mime_type"] = session.stream.value.mime_type;
  summary["bitrate_kbps"] = session.stream.value.bitrate_kbps;
  summary["duration_ms"] = session.stream.value.duration_ms;
  summary["seekable"] = session.stream.value.seekable;

  crow::json::wvalue stream;
  stream["value"] = std::move(summary);
  stream["origin"] = origin_json(session.stream.origin);

  crow::json::wvalue out;
  out["session_id"] = session.session_id;
  out["started_at_ms"] = session.started_at_ms;
  out["expires_at_ms"] = session.expires_at_ms;
  out["track"] = sourced_json(session.track, track_json);
  out["stream"] = std::move(stream);
  out["stream_url"] =
      "/api/v1/playback/" + session.session_id + "/stream";
  return out;
}

// ---------------------------------------------------------------------------
// 请求参数
// ---------------------------------------------------------------------------

std::optional<std::string> text_param(const crow::request &req,
                                      const char *name) {
  if (const char *value = req.url_params.get(name)) {
    return std::string(value);
  }
  return std::nullopt;
}

int int_param(const crow::request &req, const char *name, int fallback) {
  if (const char *value = req.url_params.get(name)) {
    const int parsed = std::atoi(value);
    if (parsed > 0) {
      return parsed;
    }
  }
  return fallback;
}

bool bool_param(const crow::request &req, const char *name) {
  if (const char *value = req.url_params.get(name)) {
    const std::string text(value);
    return text == "1" || text == "true" || text == "yes";
  }
  return false;
}

std::vector<std::string> csv_param(const crow::request &req, const char *name) {
  std::vector<std::string> out;
  const std::optional<std::string> raw = text_param(req, name);
  if (!raw.has_value()) {
    return out;
  }
  std::string current;
  for (const char ch : *raw) {
    if (ch == ',') {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) {
    out.push_back(std::move(current));
  }
  return out;
}

// 参考 5.4.6：ref 必须能解析成 library_id + entity_type + entity_id。
std::optional<SourceRef> parse_ref_param(const std::string &text) {
  return parse_ref(http_util::urlDecode(text));
}

PageRequest page_of(const crow::request &req) {
  PageRequest page;
  page.limit = std::clamp(int_param(req, "limit", 50), 1, 200);
  page.cursor = text_param(req, "cursor");
  return page;
}

CallContext context_of(const crow::request &req) {
  CallContext ctx;
  if (const std::optional<std::string> locale = text_param(req, "locale");
      locale.has_value()) {
    ctx.locale = *locale;
  }
  return ctx;
}

SearchQuery query_of(const crow::request &req) {
  SearchQuery query;
  query.text = text_param(req, "q").value_or("");
  for (const std::string &name : csv_param(req, "types")) {
    const std::optional<EntityType> type = parse_entity_type(name);
    if (!type.has_value()) {
      continue;
    }
    query.types.push_back(*type);
  }
  query.artists = csv_param(req, "artists");
  query.album_artists = csv_param(req, "album_artists");
  query.page = page_of(req);
  query.locale = text_param(req, "locale");
  query.include_unplayable = bool_param(req, "include_unplayable");
  return query;
}

crow::response bad_ref() { return invalid("malformed ref"); }

} // namespace

// ---------------------------------------------------------------------------
// 库清单与能力（只查注册表，不出数据）
// ---------------------------------------------------------------------------

crow::response libraries(const crow::request &) {
  const std::shared_ptr<LibraryRegistry> registry =
      media_library::library_registry_ptr();
  if (registry == nullptr) {
    return unavailable();
  }
  LibraryDirectory directory(registry);

  crow::json::wvalue::list items;
  for (const LibraryView &view : directory.list()) {
    crow::json::wvalue entry;
    entry["library_id"] = view.library_id;
    entry["display_name"] = view.display_name;
    entry["platform"] = std::string(platform_kind_name(view.platform));
    entry["priority"] = view.priority;
    entry["enabled"] = view.enabled;
    entry["health"] = std::string(health_state_name(view.health));
    if (view.health_reason.has_value()) {
      entry["health_reason"] = std::string(error_code_name(*view.health_reason));
    } else {
      entry["health_reason"] = nullptr;
    }
    crow::json::wvalue::list supported;
    supported.reserve(view.supported.size());
    for (const CapabilityId capability : view.supported) {
      supported.push_back(std::string(capability_name(capability)));
    }
    entry["supported"] = std::move(supported);
    crow::json::wvalue::list available;
    available.reserve(view.available.size());
    for (const CapabilityId capability : view.available) {
      available.push_back(std::string(capability_name(capability)));
    }
    entry["available"] = std::move(available);
    items.push_back(std::move(entry));
  }

  crow::json::wvalue body;
  body["libraries"] = std::move(items);
  return http_util::jsonResponse(200, body.dump());
}

crow::response provider_health(const crow::request &) {
  const std::shared_ptr<LibraryRegistry> registry =
      media_library::library_registry_ptr();
  if (registry == nullptr) {
    return unavailable();
  }
  LibraryDirectory directory(registry);

  crow::json::wvalue::list items;
  for (const LibraryView &view : directory.list()) {
    crow::json::wvalue entry;
    entry["library_id"] = view.library_id;
    entry["state"] = std::string(health_state_name(view.health));
    if (view.health_reason.has_value()) {
      entry["reason"] = std::string(error_code_name(*view.health_reason));
    } else {
      entry["reason"] = nullptr;
    }
    entry["checked_at"] = view.health_checked_at;
    items.push_back(std::move(entry));
  }

  crow::json::wvalue body;
  body["providers"] = std::move(items);
  return http_util::jsonResponse(200, body.dump());
}

// ---------------------------------------------------------------------------
// 搜索 / 聚合
// ---------------------------------------------------------------------------

crow::response search(const crow::request &req) {
  const std::shared_ptr<MediaRouter> router = media_library::media_router_ptr();
  if (router == nullptr) {
    return unavailable();
  }
  const SearchQuery query = query_of(req);
  const CallContext ctx = context_of(req);

  // library_id 显式指定 -> 单源；不指定 -> 聚合。
  if (const std::optional<std::string> library_id = text_param(req, "library_id");
      library_id.has_value() && !library_id->empty()) {
    Result<Sourced<Page<SearchHit>>> result =
        router->search_in(*library_id, query, ctx);
    if (!result.ok()) {
      return fail(result.error());
    }
    crow::json::wvalue body = sourced_page_json(result.value(), hit_json);
    body["mode"] = "single";
    return http_util::jsonResponse(200, body.dump());
  }

  Result<RoutedPage<SearchHit>> result = router->search(query, ctx);
  if (!result.ok()) {
    return fail(result.error());
  }
  crow::json::wvalue body = routed_page_json(result.value(), hit_json);
  body["mode"] = "aggregate";
  return http_util::jsonResponse(200, body.dump());
}

// ---------------------------------------------------------------------------
// 曲目
// ---------------------------------------------------------------------------

namespace {

// aspect 为空串表示单源读（get_track）；否则是多源方面读（resolve_*）。
crow::response track_aspect(const crow::request &req,
                            const std::string &ref_text,
                            const std::string &aspect) {
  const std::shared_ptr<MediaRouter> router = media_library::media_router_ptr();
  if (router == nullptr) {
    return unavailable();
  }
  const std::optional<SourceRef> ref = parse_ref_param(ref_text);
  if (!ref.has_value()) {
    return bad_ref();
  }
  if (ref->entity_type != EntityType::Track) {
    return invalid("expected a track reference");
  }
  const CallContext ctx = context_of(req);

  if (aspect.empty()) {
    Result<Sourced<Track>> result = router->get_track(*ref, ctx);
    if (!result.ok()) {
      return fail(result.error());
    }
    return http_util::jsonResponse(
        200, sourced_json(result.value(), track_json).dump());
  }

  if (aspect == "metadata") {
    Result<Sourced<Track>> result = router->resolve_track(*ref, ctx);
    if (!result.ok()) {
      return fail(result.error());
    }
    return http_util::jsonResponse(
        200, sourced_json(result.value(), track_json).dump());
  }

  if (aspect == "stream") {
    Result<Sourced<PlaybackSource>> result = router->resolve_stream(*ref, ctx);
    if (!result.ok()) {
      return fail(result.error());
    }
    return http_util::jsonResponse(
        200, sourced_json(result.value(), stream_json).dump());
  }

  if (aspect == "lyrics") {
    Result<Sourced<Lyrics>> result = router->resolve_lyrics(*ref, ctx);
    if (!result.ok()) {
      return fail(result.error());
    }
    return http_util::jsonResponse(
        200, sourced_json(result.value(), lyrics_json).dump());
  }

  if (aspect == "artwork") {
    Result<Sourced<ArtworkRef>> result = router->resolve_artwork(*ref, ctx);
    if (!result.ok()) {
      return fail(result.error());
    }
    return http_util::jsonResponse(
        200, sourced_json(result.value(), artwork_json).dump());
  }

  return invalid("unknown aspect");
}

} // namespace

crow::response track(const crow::request &req, const std::string &ref_text) {
  return track_aspect(req, ref_text, text_param(req, "aspect").value_or(""));
}

crow::response track_stream(const crow::request &req, const std::string &ref) {
  const std::optional<SourceRef> parsed = parse_ref_param(ref);
  if (!parsed.has_value()) {
    return bad_ref();
  }
  if (parsed->entity_type != EntityType::Track) {
    return invalid("expected a track reference");
  }
  // 出字节：open_playback -> open_stream -> Range 输出。locator 不出进程。
  return stream_http::serve_track(req, *parsed, req.method == crow::HTTPMethod::HEAD);
}

crow::response track_lyrics(const crow::request &req, const std::string &ref) {
  return track_aspect(req, ref, "lyrics");
}

crow::response track_artwork(const crow::request &req, const std::string &ref) {
  return track_aspect(req, ref, "artwork");
}

// ---------------------------------------------------------------------------
// 专辑 / 艺术家
// ---------------------------------------------------------------------------

crow::response album(const crow::request &req, const std::string &ref_text) {
  const std::shared_ptr<MediaRouter> router = media_library::media_router_ptr();
  if (router == nullptr) {
    return unavailable();
  }
  const std::optional<SourceRef> ref = parse_ref_param(ref_text);
  if (!ref.has_value()) {
    return bad_ref();
  }
  if (ref->entity_type != EntityType::Album) {
    return invalid("expected an album reference");
  }
  Result<Sourced<Album>> result = router->get_album(*ref, context_of(req));
  if (!result.ok()) {
    return fail(result.error());
  }
  return http_util::jsonResponse(200,
                                 sourced_json(result.value(), album_json).dump());
}

crow::response album_tracks(const crow::request &req,
                            const std::string &ref_text) {
  const std::shared_ptr<MediaRouter> router = media_library::media_router_ptr();
  if (router == nullptr) {
    return unavailable();
  }
  const std::optional<SourceRef> ref = parse_ref_param(ref_text);
  if (!ref.has_value()) {
    return bad_ref();
  }
  if (ref->entity_type != EntityType::Album) {
    return invalid("expected an album reference");
  }
  Result<Sourced<Page<Track>>> result =
      router->list_album_tracks(*ref, page_of(req), context_of(req));
  if (!result.ok()) {
    return fail(result.error());
  }
  return http_util::jsonResponse(
      200, sourced_page_json(result.value(), track_json).dump());
}

crow::response artist(const crow::request &req, const std::string &ref_text) {
  const std::shared_ptr<MediaRouter> router = media_library::media_router_ptr();
  if (router == nullptr) {
    return unavailable();
  }
  const std::optional<SourceRef> ref = parse_ref_param(ref_text);
  if (!ref.has_value()) {
    return bad_ref();
  }
  if (ref->entity_type != EntityType::Artist &&
      ref->entity_type != EntityType::AlbumArtist) {
    return invalid("expected an artist reference");
  }
  Result<Sourced<Artist>> result = router->get_artist(*ref, context_of(req));
  if (!result.ok()) {
    return fail(result.error());
  }
  return http_util::jsonResponse(
      200, sourced_json(result.value(), artist_json).dump());
}

crow::response artist_albums(const crow::request &req,
                             const std::string &ref_text) {
  const std::shared_ptr<MediaRouter> router = media_library::media_router_ptr();
  if (router == nullptr) {
    return unavailable();
  }
  const std::optional<SourceRef> ref = parse_ref_param(ref_text);
  if (!ref.has_value()) {
    return bad_ref();
  }
  if (ref->entity_type != EntityType::Artist &&
      ref->entity_type != EntityType::AlbumArtist) {
    return invalid("expected an artist reference");
  }
  Result<Sourced<Page<Album>>> result =
      router->list_artist_albums(*ref, page_of(req), context_of(req));
  if (!result.ok()) {
    return fail(result.error());
  }
  return http_util::jsonResponse(
      200, sourced_page_json(result.value(), album_json).dump());
}

// ---------------------------------------------------------------------------
// 歌单（当前仅声明了能力的库能回答）
// ---------------------------------------------------------------------------

crow::response playlists(const crow::request &req) {
  const std::shared_ptr<MediaRouter> router = media_library::media_router_ptr();
  if (router == nullptr) {
    return unavailable();
  }
  // 单源接口：库必须显式给出，不推断、不遍历。
  const std::optional<std::string> library_id = text_param(req, "library_id");
  if (!library_id.has_value() || library_id->empty()) {
    return invalid("library_id is required");
  }
  Result<Sourced<Page<Playlist>>> result = router->list_playlists(
      *library_id, page_of(req), context_of(req));
  if (!result.ok()) {
    return fail(result.error());
  }
  return http_util::jsonResponse(
      200, sourced_page_json(result.value(), playlist_json).dump());
}

crow::response playlist(const crow::request &req, const std::string &ref_text) {
  const std::shared_ptr<MediaRouter> router = media_library::media_router_ptr();
  if (router == nullptr) {
    return unavailable();
  }
  const std::optional<SourceRef> ref = parse_ref_param(ref_text);
  if (!ref.has_value()) {
    return bad_ref();
  }
  if (ref->entity_type != EntityType::Playlist) {
    return invalid("expected a playlist reference");
  }
  Result<Sourced<Playlist>> result = router->get_playlist(*ref, context_of(req));
  if (!result.ok()) {
    return fail(result.error());
  }
  return http_util::jsonResponse(
      200, sourced_json(result.value(), playlist_json).dump());
}

// ---------------------------------------------------------------------------
// 播放会话（7.1–7.4）。字节只从会话出；locator 不出进程。
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<PlaybackService> playback_or_null() {
  return media_library::playback_service_ptr();
}

} // namespace

crow::response playback_open(const crow::request &req,
                             const std::string &ref_text) {
  const std::shared_ptr<PlaybackService> service = playback_or_null();
  if (service == nullptr) {
    return unavailable();
  }
  const std::optional<SourceRef> ref = parse_ref_param(ref_text);
  if (!ref.has_value()) {
    return bad_ref();
  }
  if (ref->entity_type != EntityType::Track) {
    return invalid("expected a track reference");
  }
  Result<PlaybackSession> session = service->open_playback(*ref, context_of(req));
  if (!session.ok()) {
    return fail(session.error());
  }
  return http_util::jsonResponse(201, session_json(session.value()).dump());
}

crow::response playback_session(const crow::request &,
                                const std::string &session_id) {
  const std::shared_ptr<PlaybackService> service = playback_or_null();
  if (service == nullptr) {
    return unavailable();
  }
  Result<PlaybackSession> session = service->session(session_id);
  if (!session.ok()) {
    return fail(session.error());
  }
  return http_util::jsonResponse(200, session_json(session.value()).dump());
}

crow::response playback_refresh(const crow::request &req,
                                const std::string &session_id) {
  const std::shared_ptr<PlaybackService> service = playback_or_null();
  if (service == nullptr) {
    return unavailable();
  }
  Result<PlaybackSession> session =
      service->refresh(session_id, context_of(req));
  if (!session.ok()) {
    return fail(session.error());
  }
  return http_util::jsonResponse(200, session_json(session.value()).dump());
}

crow::response playback_close(const crow::request &,
                              const std::string &session_id) {
  const std::shared_ptr<PlaybackService> service = playback_or_null();
  if (service == nullptr) {
    return unavailable();
  }
  Status status = service->close(session_id);
  if (!status.ok()) {
    return fail(status.error());
  }
  return crow::response(204);
}

crow::response playback_stream(const crow::request &req,
                               const std::string &session_id) {
  return stream_http::serve_session(req, session_id, req.method == crow::HTTPMethod::HEAD);
}

} // namespace api_v1
