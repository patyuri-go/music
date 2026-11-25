#include "api/v1/v1.h"

#include <json/json.h>

#include "json_util.h"
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

http_server::ResponsePtr fail(const Error &error) {
  return stream_http::error_response(error);
}

http_server::ResponsePtr invalid(const std::string &message) {
  return fail(Error::make(ErrorCode::InvalidRequest, message));
}

http_server::ResponsePtr unavailable() {
  return fail(Error::make(ErrorCode::ProviderUnavailable,
                          "media runtime is not initialized"));
}

// ---------------------------------------------------------------------------
// 领域模型序列化
// ---------------------------------------------------------------------------

Json::Value ref_json(const SourceRef &ref) {
  Json::Value out;
  out["ref"] = to_string(ref);
  out["library_id"] = ref.library_id;
  out["type"] = std::string(entity_type_name(ref.entity_type));
  out["id"] = ref.entity_id;
  return out;
}

Json::Value restrictions_json(const std::vector<Restriction> &items) {
  Json::Value out(Json::arrayValue);
  for (const Restriction &item : items) {
    Json::Value entry;
    entry["code"] = std::string(restriction_code_name(item.code));
    entry["message"] = item.message;
    out.append(std::move(entry));
  }
  return Json::Value(std::move(out));
}

Json::Value artwork_json(const ArtworkRef &artwork) {
  Json::Value out;
  out["owner"] = ref_json(artwork.owner);
  out["kind"] = artwork.kind;
  if (artwork.artwork_id.has_value()) {
    out["artwork_id"] = *artwork.artwork_id;
  } else {
    out["artwork_id"] = Json::Value(Json::nullValue);
  }
  out["cache_tag"] = artwork.cache_tag;
  return out;
}

Json::Value artist_links_json(const std::vector<ArtistLink> &items) {
  Json::Value out(Json::arrayValue);
  for (const ArtistLink &item : items) {
    Json::Value entry;
    entry["name"] = item.name;
    entry["artist"] = ref_json(item.ref);
    out.append(std::move(entry));
  }
  return Json::Value(std::move(out));
}

Json::Value album_artist_links_json(
    const std::vector<AlbumArtistLink> &items) {
  Json::Value out(Json::arrayValue);
  for (const AlbumArtistLink &item : items) {
    Json::Value entry;
    entry["name"] = item.name;
    entry["album_artist"] = ref_json(item.ref);
    out.append(std::move(entry));
  }
  return Json::Value(std::move(out));
}

Json::Value track_json(const Track &track) {
  Json::Value out;
  out["ref"] = ref_json(track.ref);
  out["title"] = track.title;
  out["artists"] = artist_links_json(track.artists);
  out["album_artists"] = album_artist_links_json(track.album_artists);
  if (track.album.has_value()) {
    Json::Value album;
    album["title"] = track.album->title;
    album["album"] = ref_json(track.album->ref);
    out["album"] = std::move(album);
  } else {
    out["album"] = Json::Value(Json::nullValue);
  }
  out["date"] = track.date.has_value() ? Json::Value(*track.date)
                                       : Json::Value(Json::nullValue);
  out["duration_ms"] = track.duration_ms.has_value()
                           ? Json::Value(*track.duration_ms)
                           : Json::Value(Json::nullValue);
  out["track_no"] = track.track_no.has_value()
                        ? Json::Value(*track.track_no)
                        : Json::Value(Json::nullValue);
  out["disc_no"] = track.disc_no.has_value()
                       ? Json::Value(*track.disc_no)
                       : Json::Value(Json::nullValue);
  out["year"] = track.year.has_value() ? Json::Value(*track.year)
                                       : Json::Value(Json::nullValue);

  Json::Value genres(Json::arrayValue);
  for (const std::string &genre : track.genres) {
    genres.append(genre);
  }
  out["genres"] = std::move(genres);

  out["artwork"] = track.artwork.has_value()
                       ? artwork_json(*track.artwork)
                       : Json::Value(Json::nullValue);
  out["lyrics_available"] = track.lyrics_available;
  out["playable"] = track.playable;
  out["restrictions"] = restrictions_json(track.restrictions);

  Json::Value audio;
  audio["bitrate_kbps"] = track.audio.bitrate_kbps.has_value()
                              ? Json::Value(*track.audio.bitrate_kbps)
                              : Json::Value(Json::nullValue);
  audio["sample_rate"] = track.audio.sample_rate.has_value()
                             ? Json::Value(*track.audio.sample_rate)
                             : Json::Value(Json::nullValue);
  audio["channels"] = track.audio.channels.has_value()
                          ? Json::Value(*track.audio.channels)
                          : Json::Value(Json::nullValue);
  audio["format"] = track.audio.format;
  audio["size_bytes"] = track.audio.size_bytes.has_value()
                            ? Json::Value(*track.audio.size_bytes)
                            : Json::Value(Json::nullValue);
  out["audio"] = std::move(audio);
  return out;
}

Json::Value album_json(const Album &album) {
  Json::Value out;
  out["ref"] = ref_json(album.ref);
  out["title"] = album.title;
  out["artists"] = album_artist_links_json(album.artists);
  out["date"] = album.date.has_value() ? Json::Value(*album.date)
                                       : Json::Value(Json::nullValue);
  out["year"] = album.year.has_value() ? Json::Value(*album.year)
                                       : Json::Value(Json::nullValue);
  Json::Value genres(Json::arrayValue);
  for (const std::string &genre : album.genres) {
    genres.append(genre);
  }
  out["genres"] = std::move(genres);
  out["artwork"] = album.artwork.has_value()
                       ? artwork_json(*album.artwork)
                       : Json::Value(Json::nullValue);
  return out;
}

Json::Value artist_json(const Artist &artist) {
  Json::Value out;
  out["ref"] = ref_json(artist.ref);
  out["name"] = artist.name;
  out["sort_name"] = artist.sort_name;
  out["artwork"] = artist.artwork.has_value()
                       ? artwork_json(*artist.artwork)
                       : Json::Value(Json::nullValue);
  return out;
}

Json::Value playlist_json(const Playlist &playlist) {
  Json::Value out;
  out["ref"] = ref_json(playlist.ref);
  out["name"] = playlist.name;
  out["description"] = playlist.description;
  Json::Value tracks(Json::arrayValue);
  for (const Track &track : playlist.tracks) {
    tracks.append(to_string(track.ref));
  }
  out["tracks"] = std::move(tracks);
  return out;
}

Json::Value lyrics_json(const Lyrics &lyrics) {
  Json::Value out;
  out["track"] = ref_json(lyrics.track);
  out["language"] = lyrics.language.has_value()
                        ? Json::Value(*lyrics.language)
                        : Json::Value(Json::nullValue);
  Json::Value modes(Json::arrayValue);
  for (LyricsMode mode : lyrics.modes) {
    switch (mode) {
    case LyricsMode::WordSynced:
      modes.append("word_synced");
      break;
    case LyricsMode::LineSynced:
      modes.append("line_synced");
      break;
    case LyricsMode::Unsynced:
      modes.append("unsynced");
      break;
    case LyricsMode::HasTranslation:
      modes.append("has_translation");
      break;
    case LyricsMode::HasRomanization:
      modes.append("has_romanization");
      break;
    case LyricsMode::HasBackground:
      modes.append("has_background");
      break;
    }
  }
  out["modes"] = std::move(modes);
  Json::Value lines(Json::arrayValue);
  for (const LyricLine &line : lyrics.lines) {
    Json::Value entry;
    entry["start_ms"] = line.start_ms.has_value()
                            ? Json::Value(*line.start_ms)
                            : Json::Value(Json::nullValue);
    entry["end_ms"] = line.end_ms.has_value() ? Json::Value(*line.end_ms)
                                              : Json::Value(Json::nullValue);
    entry["text"] = line_text(line);
    const std::vector<LyricWord> line_word_list = line_words(line);
    Json::Value words(Json::arrayValue);
    for (const LyricWord &word : line_word_list) {
      Json::Value item;
      item["start_ms"] = word.start_ms;
      item["end_ms"] = word.end_ms.has_value()
                           ? Json::Value(*word.end_ms)
                           : Json::Value(Json::nullValue);
      item["text"] = word.text;
      words.append(std::move(item));
    }
    entry["words"] = std::move(words);
    Json::Value aux(Json::arrayValue);
    for (const LyricItem &item : line.items) {
      if (item.kind != LyricItemKind::Translation &&
          item.kind != LyricItemKind::Romanization) {
        continue;
      }
      Json::Value node;
      node["kind"] = item.kind == LyricItemKind::Translation ? "translation"
                                                             : "romanization";
      node["text"] = item.text;
      const std::vector<LyricWord> aux_word_list = item_words(item);
      Json::Value aux_words(Json::arrayValue);
      for (const LyricWord &word : aux_word_list) {
        Json::Value w;
        w["start_ms"] = word.start_ms;
        w["end_ms"] = word.end_ms.has_value() ? Json::Value(*word.end_ms)
                                              : Json::Value(Json::nullValue);
        w["text"] = word.text;
        aux_words.append(std::move(w));
      }
      node["words"] = std::move(aux_words);
      aux.append(std::move(node));
    }
    entry["aux"] = std::move(aux);
    lines.append(std::move(entry));
  }
  out["lines"] = std::move(lines);
  out["plain_text"] = lyrics.plain_text.has_value()
                          ? Json::Value(*lyrics.plain_text)
                          : Json::Value(Json::nullValue);
  return out;
}

// internal_locator 是进程内定位符，绝不写进响应（5.4.4）。
Json::Value stream_json(const PlaybackSource &source) {
  Json::Value out;
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
                       ? Json::Value(*source.quality)
                       : Json::Value(Json::nullValue);
  out["bitrate_kbps"] = source.bitrate_kbps.has_value()
                            ? Json::Value(*source.bitrate_kbps)
                            : Json::Value(Json::nullValue);
  out["mime_type"] = source.mime_type;
  out["content_length"] = source.content_length.has_value()
                              ? Json::Value(*source.content_length)
                              : Json::Value(Json::nullValue);
  out["range_supported"] = source.range_supported;
  out["expires_at"] = source.expires_at.has_value()
                          ? Json::Value(*source.expires_at)
                          : Json::Value(Json::nullValue);
  out["access_mode"] =
      source.access_mode == AccessMode::Redirect ? "redirect" : "proxy";
  out["cacheable"] = source.cacheable;
  out["restrictions"] = restrictions_json(source.restrictions);
  return out;
}

Json::Value hit_json(const SearchHit &hit) {
  Json::Value out;
  out["kind"] = std::string(entity_type_name(hit.kind));
  out["ref"] = ref_json(hit.ref);
  out["title"] = hit.title;
  out["subtitle"] = hit.subtitle.has_value()
                        ? Json::Value(*hit.subtitle)
                        : Json::Value(Json::nullValue);
  out["artists"] = artist_links_json(hit.artists);
  out["album_artists"] = album_artist_links_json(hit.album_artists);
  // 所属专辑：曲目命中靠它挂进「专辑艺术家 → 专辑 → 曲目」树；专辑命中
  // 自身就是专辑节点，这里为 null，由调用方用 ref 顶替。
  if (hit.album.has_value()) {
    Json::Value album;
    album["title"] = hit.album->title;
    album["album"] = ref_json(hit.album->ref);
    out["album"] = std::move(album);
  } else {
    out["album"] = Json::Value(Json::nullValue);
  }
  out["artwork"] = hit.artwork.has_value() ? artwork_json(*hit.artwork)
                                           : Json::Value(Json::nullValue);
  out["duration_ms"] = hit.duration_ms.has_value()
                           ? Json::Value(*hit.duration_ms)
                           : Json::Value(Json::nullValue);
  out["playable"] = hit.playable;
  return out;
}

// ---------------------------------------------------------------------------
// 信封序列化
// ---------------------------------------------------------------------------

Json::Value origin_json(const Origin &origin) {
  Json::Value out;
  out["library_id"] = origin.library_id;
  out["ref"] = to_string(origin.ref);
  if (origin.aspect.has_value()) {
    out["aspect"] = std::string(aspect_name(*origin.aspect));
  } else {
    out["aspect"] = Json::Value(Json::nullValue);
  }
  out["substituted"] = origin.substituted;
  Json::Value chain(Json::arrayValue);
  for (const RouteAttempt &attempt : origin.chain) {
    Json::Value hop;
    hop["library_id"] = attempt.library_id;
    hop["ok"] = attempt.ok;
    if (attempt.error.has_value()) {
      hop["error"] = std::string(error_code_name(*attempt.error));
    } else {
      hop["error"] = Json::Value(Json::nullValue);
    }
    hop["elapsed_ms"] = attempt.elapsed_ms;
    chain.append(std::move(hop));
  }
  out["chain"] = std::move(chain);
  return out;
}

Json::Value sources_json(const std::vector<SourceStatus> &sources) {
  Json::Value out(Json::arrayValue);
  for (const SourceStatus &status : sources) {
    Json::Value entry;
    entry["library_id"] = status.library_id;
    entry["state"] = std::string(source_state_name(status.state));
    if (status.error.has_value()) {
      entry["error"] = std::string(error_code_name(status.error->code));
    } else {
      entry["error"] = Json::Value(Json::nullValue);
    }
    entry["item_count"] = status.item_count;
    entry["elapsed_ms"] = status.elapsed_ms;
    out.append(std::move(entry));
  }
  return Json::Value(std::move(out));
}

template <typename T, typename Fn>
Json::Value sourced_json(const Sourced<T> &sourced, Fn fn) {
  Json::Value out;
  out["value"] = fn(sourced.value);
  out["origin"] = origin_json(sourced.origin);
  return out;
}

template <typename T, typename Fn>
Json::Value sourced_page_json(const Sourced<Page<T>> &sourced, Fn fn) {
  Json::Value page;
  Json::Value items(Json::arrayValue);
  for (const T &item : sourced.value.items) {
    items.append(fn(item));
  }
  page["items"] = std::move(items);
  page["next_cursor"] = sourced.value.next_cursor.has_value()
                            ? Json::Value(*sourced.value.next_cursor)
                            : Json::Value(Json::nullValue);
  page["has_more"] = sourced.value.has_more;

  Json::Value out;
  out["value"] = std::move(page);
  out["origin"] = origin_json(sourced.origin);
  return out;
}

template <typename T, typename Fn>
Json::Value routed_page_json(const RoutedPage<T> &page, Fn fn) {
  Json::Value out;
  Json::Value items(Json::arrayValue);
  for (const Sourced<T> &item : page.items) {
    items.append(sourced_json(item, fn));
  }
  out["items"] = std::move(items);
  out["next_cursor"] = page.next_cursor.has_value()
                           ? Json::Value(*page.next_cursor)
                           : Json::Value(Json::nullValue);
  out["has_more"] = page.has_more;
  out["sources"] = sources_json(page.sources);
  return out;
}

// 7.1 会话序列化：这里没有 locator，也没有 internal_locator 的任何形态。
Json::Value session_json(const PlaybackSession &session) {
  Json::Value summary;
  summary["track"] = ref_json(session.stream.value.track);
  summary["mime_type"] = session.stream.value.mime_type;
  summary["bitrate_kbps"] = session.stream.value.bitrate_kbps;
  summary["duration_ms"] = session.stream.value.duration_ms;
  summary["seekable"] = session.stream.value.seekable;

  Json::Value stream;
  stream["value"] = std::move(summary);
  stream["origin"] = origin_json(session.stream.origin);

  Json::Value out;
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

std::optional<std::string> text_param(const http_server::RequestPtr &req,
                                      const char *name) {
  return http_server::Query(req).get(name);
}

int int_param(const http_server::RequestPtr &req, const char *name, int fallback) {
  const std::optional<std::string> value = text_param(req, name);
  if (value.has_value()) {
    const int parsed = std::atoi(value->c_str());
    if (parsed > 0) {
      return parsed;
    }
  }
  return fallback;
}

bool bool_param(const http_server::RequestPtr &req, const char *name) {
  const std::optional<std::string> value = text_param(req, name);
  if (value.has_value()) {
    return *value == "1" || *value == "true" || *value == "yes";
  }
  return false;
}

std::vector<std::string> csv_param(const http_server::RequestPtr &req, const char *name) {
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

PageRequest page_of(const http_server::RequestPtr &req) {
  PageRequest page;
  page.limit = std::clamp(int_param(req, "limit", 50), 1, 200);
  page.cursor = text_param(req, "cursor");
  return page;
}

CallContext context_of(const http_server::RequestPtr &req) {
  CallContext ctx;
  if (const std::optional<std::string> locale = text_param(req, "locale");
      locale.has_value()) {
    ctx.locale = *locale;
  }
  return ctx;
}

SearchQuery query_of(const http_server::RequestPtr &req) {
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

http_server::ResponsePtr bad_ref() { return invalid("malformed ref"); }

} // namespace

// ---------------------------------------------------------------------------
// 库清单与能力（只查注册表，不出数据）
// ---------------------------------------------------------------------------

http_server::ResponsePtr libraries(const http_server::RequestPtr &) {
  const std::shared_ptr<LibraryRegistry> registry =
      media_library::library_registry_ptr();
  if (registry == nullptr) {
    return unavailable();
  }
  LibraryDirectory directory(registry);

  Json::Value items(Json::arrayValue);
  for (const LibraryView &view : directory.list()) {
    Json::Value entry;
    entry["library_id"] = view.library_id;
    entry["display_name"] = view.display_name;
    entry["platform"] = std::string(platform_kind_name(view.platform));
    entry["priority"] = view.priority;
    entry["enabled"] = view.enabled;
    entry["health"] = std::string(health_state_name(view.health));
    if (view.health_reason.has_value()) {
      entry["health_reason"] = std::string(error_code_name(*view.health_reason));
    } else {
      entry["health_reason"] = Json::Value(Json::nullValue);
    }
    Json::Value supported(Json::arrayValue);
    for (const CapabilityId capability : view.supported) {
      supported.append(std::string(capability_name(capability)));
    }
    entry["supported"] = std::move(supported);
    Json::Value available(Json::arrayValue);
    for (const CapabilityId capability : view.available) {
      available.append(std::string(capability_name(capability)));
    }
    entry["available"] = std::move(available);
    items.append(std::move(entry));
  }

  Json::Value body;
  body["libraries"] = std::move(items);
  return http_util::jsonResponse(200, json_util::dump(body));
}

http_server::ResponsePtr provider_health(const http_server::RequestPtr &) {
  const std::shared_ptr<LibraryRegistry> registry =
      media_library::library_registry_ptr();
  if (registry == nullptr) {
    return unavailable();
  }
  LibraryDirectory directory(registry);

  Json::Value items(Json::arrayValue);
  for (const LibraryView &view : directory.list()) {
    Json::Value entry;
    entry["library_id"] = view.library_id;
    entry["state"] = std::string(health_state_name(view.health));
    if (view.health_reason.has_value()) {
      entry["reason"] = std::string(error_code_name(*view.health_reason));
    } else {
      entry["reason"] = Json::Value(Json::nullValue);
    }
    entry["checked_at"] = view.health_checked_at;
    items.append(std::move(entry));
  }

  Json::Value body;
  body["providers"] = std::move(items);
  return http_util::jsonResponse(200, json_util::dump(body));
}

// ---------------------------------------------------------------------------
// 搜索 / 聚合
// ---------------------------------------------------------------------------

http_server::ResponsePtr search(const http_server::RequestPtr &req) {
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
    Json::Value body = sourced_page_json(result.value(), hit_json);
    body["mode"] = "single";
    return http_util::jsonResponse(200, json_util::dump(body));
  }

  Result<RoutedPage<SearchHit>> result = router->search(query, ctx);
  if (!result.ok()) {
    return fail(result.error());
  }
  Json::Value body = routed_page_json(result.value(), hit_json);
  body["mode"] = "aggregate";
  return http_util::jsonResponse(200, json_util::dump(body));
}

// ---------------------------------------------------------------------------
// 曲目
// ---------------------------------------------------------------------------

namespace {

// aspect 为空串表示单源读（get_track）；否则是多源方面读（resolve_*）。
http_server::ResponsePtr track_aspect(const http_server::RequestPtr &req,
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
        200, json_util::dump(sourced_json(result.value(), track_json)));
  }

  if (aspect == "metadata") {
    Result<Sourced<Track>> result = router->resolve_track(*ref, ctx);
    if (!result.ok()) {
      return fail(result.error());
    }
    return http_util::jsonResponse(
        200, json_util::dump(sourced_json(result.value(), track_json)));
  }

  if (aspect == "stream") {
    Result<Sourced<PlaybackSource>> result = router->resolve_stream(*ref, ctx);
    if (!result.ok()) {
      return fail(result.error());
    }
    return http_util::jsonResponse(
        200, json_util::dump(sourced_json(result.value(), stream_json)));
  }

  if (aspect == "lyrics") {
    Result<Sourced<Lyrics>> result = router->resolve_lyrics(*ref, ctx);
    if (!result.ok()) {
      return fail(result.error());
    }
    return http_util::jsonResponse(
        200, json_util::dump(sourced_json(result.value(), lyrics_json)));
  }

  if (aspect == "artwork") {
    Result<Sourced<ArtworkRef>> result = router->resolve_artwork(*ref, ctx);
    if (!result.ok()) {
      return fail(result.error());
    }
    return http_util::jsonResponse(
        200, json_util::dump(sourced_json(result.value(), artwork_json)));
  }

  return invalid("unknown aspect");
}

} // namespace

http_server::ResponsePtr track(const http_server::RequestPtr &req, const std::string &ref_text) {
  return track_aspect(req, ref_text, text_param(req, "aspect").value_or(""));
}

http_server::ResponsePtr track_stream(const http_server::RequestPtr &req, const std::string &ref) {
  const std::optional<SourceRef> parsed = parse_ref_param(ref);
  if (!parsed.has_value()) {
    return bad_ref();
  }
  if (parsed->entity_type != EntityType::Track) {
    return invalid("expected a track reference");
  }
  // 出字节：open_playback -> open_stream -> Range 输出。locator 不出进程。
  return stream_http::serve_track(req, *parsed, req && req->method() == drogon::Head);
}

http_server::ResponsePtr track_lyrics(const http_server::RequestPtr &req, const std::string &ref) {
  return track_aspect(req, ref, "lyrics");
}

http_server::ResponsePtr track_artwork(const http_server::RequestPtr &req, const std::string &ref) {
  // 字节出口：owner 是曲目本身，由 ArtworkService 经 MediaRouter 选源后
  // 落到对应库的 ArtworkProvider/ArtworkBytes；locator 不出进程。
  const std::optional<SourceRef> parsed = parse_ref_param(ref);
  if (!parsed.has_value()) {
    return bad_ref();
  }
  if (parsed->entity_type != EntityType::Track) {
    return invalid("expected a track reference");
  }
  media_library::ArtworkRef artwork;
  artwork.owner = *parsed;
  return stream_http::serve_artwork(req, artwork,
                                    req && req->method() == drogon::Head);
}

// ---------------------------------------------------------------------------
// 专辑 / 艺术家
// ---------------------------------------------------------------------------

http_server::ResponsePtr album(const http_server::RequestPtr &req, const std::string &ref_text) {
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
                                 json_util::dump(sourced_json(result.value(), album_json)));
}

http_server::ResponsePtr album_tracks(const http_server::RequestPtr &req,
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
      200, json_util::dump(sourced_page_json(result.value(), track_json)));
}

http_server::ResponsePtr artist(const http_server::RequestPtr &req, const std::string &ref_text) {
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
      200, json_util::dump(sourced_json(result.value(), artist_json)));
}

http_server::ResponsePtr artist_albums(const http_server::RequestPtr &req,
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
      200, json_util::dump(sourced_page_json(result.value(), album_json)));
}

// ---------------------------------------------------------------------------
// 歌单：读操作走播放列表总管（与 MediaRouter 并列的另一条入口）。
// 写操作不存在——没有 PlaylistWriter 能力。
// ---------------------------------------------------------------------------

http_server::ResponsePtr playlists(const http_server::RequestPtr &req) {
  // library_id 显式指定 -> 单源（路由器）；不指定 -> 总管跨库汇总。
  if (const std::optional<std::string> library_id = text_param(req, "library_id");
      library_id.has_value() && !library_id->empty()) {
    const std::shared_ptr<MediaRouter> router =
        media_library::media_router_ptr();
    if (router == nullptr) {
      return unavailable();
    }
    Result<Sourced<Page<Playlist>>> result = router->list_playlists(
        *library_id, page_of(req), context_of(req));
    if (!result.ok()) {
      return fail(result.error());
    }
    Json::Value body = sourced_page_json(result.value(), playlist_json);
    body["mode"] = "single";
    return http_util::jsonResponse(200, json_util::dump(body));
  }

  const std::shared_ptr<media_library::PlaylistDirectory> directory =
      media_library::playlist_directory_ptr();
  if (directory == nullptr) {
    return unavailable();
  }
  Result<RoutedPage<Playlist>> result =
      directory->list_playlists(page_of(req), context_of(req));
  if (!result.ok()) {
    return fail(result.error());
  }
  Json::Value body = routed_page_json(result.value(), playlist_json);
  body["mode"] = "aggregate";
  return http_util::jsonResponse(200, json_util::dump(body));
}

http_server::ResponsePtr playlist(const http_server::RequestPtr &req, const std::string &ref_text) {
  const std::shared_ptr<media_library::PlaylistDirectory> directory =
      media_library::playlist_directory_ptr();
  if (directory == nullptr) {
    return unavailable();
  }
  const std::optional<SourceRef> ref = parse_ref_param(ref_text);
  if (!ref.has_value()) {
    return bad_ref();
  }
  if (ref->entity_type != EntityType::Playlist) {
    return invalid("expected a playlist reference");
  }
  Result<Sourced<Playlist>> result =
      directory->get_playlist(*ref, context_of(req));
  if (!result.ok()) {
    return fail(result.error());
  }
  return http_util::jsonResponse(
      200, json_util::dump(sourced_json(result.value(), playlist_json)));
}

// ---------------------------------------------------------------------------
// 实体链接（EntityLink）：以 SourceRef 为句柄，内部 link_id 不出进程。
// ---------------------------------------------------------------------------

namespace {

Json::Value linked_ref_json(const LinkedRef &item) {
  Json::Value out;
  out["ref"] = to_string(item.ref);
  out["library_id"] = item.ref.library_id;
  out["type"] = std::string(entity_type_name(item.ref.entity_type));
  out["id"] = item.ref.entity_id;
  out["confidence"] = item.confidence;
  out["verified"] = item.verified;
  return out;
}

Json::Value overrides_json(const LinkOverrides &overrides) {
  Json::Value out = Json::Value(Json::objectValue);
  for (const auto &entry : overrides.order) {
    Json::Value libraries(Json::arrayValue);
    for (const std::string &library_id : entry.second) {
      libraries.append(library_id);
    }
    out[std::string(aspect_name(entry.first))] =
        Json::Value(std::move(libraries));
  }
  return out;
}

Json::Value link_view_json(const EntityLinkView &view) {
  Json::Value out;
  out["entity_type"] = std::string(entity_type_name(view.entity_type));
  Json::Value refs(Json::arrayValue);
  for (const LinkedRef &item : view.refs) {
    refs.append(linked_ref_json(item));
  }
  out["refs"] = std::move(refs);
  out["overrides"] = overrides_json(view.overrides);
  out["created_at"] = view.created_at;
  out["updated_at"] = view.updated_at;
  return out;
}

constexpr Aspect kAllAspects[] = {Aspect::Metadata, Aspect::Lyrics,
                                  Aspect::Stream, Aspect::Artwork};

std::string ref_of_library(const EntityLinkView &view,
                           const std::string &library_id) {
  for (const LinkedRef &item : view.refs) {
    if (item.ref.library_id == library_id) {
      return to_string(item.ref);
    }
  }
  return std::string();
}

Json::Value order_json(EntityLinkResolver &resolver,
                              const SourceRef &anchor,
                              const EntityLinkView &view,
                              const std::vector<Aspect> &aspects) {
  Json::Value out = Json::Value(Json::objectValue);
  for (Aspect aspect : aspects) {
    Json::Value entries(Json::arrayValue);
    for (const std::string &library_id :
         resolver.effective_order(anchor, aspect)) {
      Json::Value entry;
      entry["library_id"] = library_id;
      const std::string ref = ref_of_library(view, library_id);
      if (!ref.empty()) {
        entry["ref"] = ref;
      }
      entries.append(std::move(entry));
    }
    out[std::string(aspect_name(aspect))] =
        Json::Value(std::move(entries));
  }
  return out;
}

std::optional<SourceRef> ref_from_body(const Json::Value &body,
                                       const char *field) {
  if (!body.isMember(field)) {
    return std::nullopt;
  }
  const Json::Value &value = body[field];
  if (!value.isString()) {
    return std::nullopt;
  }
  return parse_ref(std::string(value.asString()));
}

std::optional<LinkOverrides> overrides_from_body(const Json::Value &body,
                                                 std::string &error) {
  if (!body.isMember("order")) {
    error = "body requires an order object";
    return std::nullopt;
  }
  const Json::Value &order = body["order"];
  if (!order.isObject()) {
    error = "order must be an object";
    return std::nullopt;
  }
  LinkOverrides overrides;
  for (const std::string &name : order.getMemberNames()) {
    const std::optional<Aspect> aspect = parse_aspect(name);
    if (!aspect.has_value()) {
      error = "unknown aspect: " + name;
      return std::nullopt;
    }
    const Json::Value &values = order[name];
    if (!values.isArray()) {
      error = "aspect " + name + " must be a list of library ids";
      return std::nullopt;
    }
    std::vector<std::string> libraries;
    for (Json::ArrayIndex i = 0; i < values.size(); ++i) {
      const Json::Value &value = values[i];
      if (!value.isString()) {
        error = "aspect " + name + " must be a list of library ids";
        return std::nullopt;
      }
      libraries.emplace_back(value.asString());
    }
    overrides.order[*aspect] = std::move(libraries);
  }
  return overrides;
}

} // namespace

http_server::ResponsePtr entity_link(const http_server::RequestPtr &req,
                           const std::string &ref_text) {
  const std::shared_ptr<EntityLinkResolver> resolver = entity_links_ptr();
  if (resolver == nullptr) {
    return unavailable();
  }
  const std::optional<SourceRef> ref = parse_ref_param(ref_text);
  if (!ref.has_value()) {
    return bad_ref();
  }
  Result<EntityLinkView> view = resolver->find_link(*ref, context_of(req));
  if (!view.ok()) {
    return fail(view.error());
  }
  return http_util::jsonResponse(200, json_util::dump(link_view_json(view.value())));
}

http_server::ResponsePtr entity_link_order(const http_server::RequestPtr &req,
                                 const std::string &ref_text) {
  const std::shared_ptr<EntityLinkResolver> resolver = entity_links_ptr();
  if (resolver == nullptr) {
    return unavailable();
  }
  const std::optional<SourceRef> ref = parse_ref_param(ref_text);
  if (!ref.has_value()) {
    return bad_ref();
  }
  Result<EntityLinkView> view = resolver->find_link(*ref, context_of(req));
  if (!view.ok()) {
    return fail(view.error());
  }

  std::vector<Aspect> aspects;
  if (const std::optional<std::string> requested = text_param(req, "aspect");
      requested.has_value()) {
    const std::optional<Aspect> aspect = parse_aspect(*requested);
    if (!aspect.has_value()) {
      return invalid("unknown aspect: " + *requested);
    }
    if (!aspect_accepts(*aspect, view.value().entity_type)) {
      return invalid("aspect not supported for this entity type");
    }
    aspects.push_back(*aspect);
  } else {
    for (Aspect aspect : kAllAspects) {
      if (aspect_accepts(aspect, view.value().entity_type)) {
        aspects.push_back(aspect);
      }
    }
  }

  Json::Value body;
  body["order"] = order_json(*resolver, *ref, view.value(), aspects);
  return http_util::jsonResponse(200, json_util::dump(body));
}

http_server::ResponsePtr entity_link_create(const http_server::RequestPtr &req) {
  const std::shared_ptr<EntityLinkResolver> resolver = entity_links_ptr();
  if (resolver == nullptr) {
    return unavailable();
  }
  const Json::Value body = json_util::parseOrNull(req->body());
  if (body.isNull() || !body.isObject()) {
    return invalid("body must be a JSON object");
  }
  const std::optional<SourceRef> a = ref_from_body(body, "a");
  const std::optional<SourceRef> b = ref_from_body(body, "b");
  if (!a.has_value() || !b.has_value()) {
    return invalid("body requires string refs a and b");
  }
  const CallContext ctx = context_of(req);
  const Status status = resolver->link(*a, *b, ctx);
  if (!status.ok()) {
    return fail(status.error());
  }
  Result<EntityLinkView> view = resolver->find_link(*a, ctx);
  if (!view.ok()) {
    return fail(view.error());
  }
  return http_util::jsonResponse(200, json_util::dump(link_view_json(view.value())));
}

http_server::ResponsePtr entity_link_delete(const http_server::RequestPtr &req,
                                  const std::string &ref_text) {
  const std::shared_ptr<EntityLinkResolver> resolver = entity_links_ptr();
  if (resolver == nullptr) {
    return unavailable();
  }
  const std::optional<SourceRef> ref = parse_ref_param(ref_text);
  if (!ref.has_value()) {
    return bad_ref();
  }
  const Status status = resolver->delete_link(*ref, context_of(req));
  if (!status.ok()) {
    return fail(status.error());
  }
  return http_server::textResponse(204, "");
}

http_server::ResponsePtr entity_link_remove_member(const http_server::RequestPtr &req,
                                         const std::string &anchor_text,
                                         const std::string &member_text) {
  const std::shared_ptr<EntityLinkResolver> resolver = entity_links_ptr();
  if (resolver == nullptr) {
    return unavailable();
  }
  const std::optional<SourceRef> anchor = parse_ref_param(anchor_text);
  const std::optional<SourceRef> member = parse_ref_param(member_text);
  if (!anchor.has_value() || !member.has_value()) {
    return bad_ref();
  }
  const CallContext ctx = context_of(req);
  Result<EntityLinkView> view = resolver->find_link(*anchor, ctx);
  if (!view.ok()) {
    return fail(view.error());
  }
  const bool present =
      std::any_of(view.value().refs.begin(), view.value().refs.end(),
                  [&member](const LinkedRef &item) {
                    return item.ref == *member;
                  });
  if (!present) {
    return fail(Error::make(ErrorCode::NotFound, "member not found in link"));
  }
  const Status status = resolver->remove_ref(*member, ctx);
  if (!status.ok()) {
    return fail(status.error());
  }
  return http_server::textResponse(204, "");
}

http_server::ResponsePtr entity_link_set_overrides(const http_server::RequestPtr &req,
                                         const std::string &ref_text) {
  const std::shared_ptr<EntityLinkResolver> resolver = entity_links_ptr();
  if (resolver == nullptr) {
    return unavailable();
  }
  const std::optional<SourceRef> ref = parse_ref_param(ref_text);
  if (!ref.has_value()) {
    return bad_ref();
  }
  const Json::Value body = json_util::parseOrNull(req->body());
  if (body.isNull() || !body.isObject()) {
    return invalid("body must be a JSON object");
  }
  std::string error;
  const std::optional<LinkOverrides> overrides =
      overrides_from_body(body, error);
  if (!overrides.has_value()) {
    return invalid(error);
  }
  const CallContext ctx = context_of(req);
  const Status status = resolver->set_overrides(*ref, *overrides, ctx);
  if (!status.ok()) {
    return fail(status.error());
  }
  Result<LinkOverrides> applied = resolver->overrides_of(*ref, ctx);
  if (!applied.ok()) {
    return fail(applied.error());
  }
  Json::Value out;
  out["overrides"] = overrides_json(applied.value());
  return http_util::jsonResponse(200, json_util::dump(out));
}

http_server::ResponsePtr entity_link_clear_override(const http_server::RequestPtr &req,
                                          const std::string &ref_text,
                                          const std::string &aspect_text) {
  const std::shared_ptr<EntityLinkResolver> resolver = entity_links_ptr();
  if (resolver == nullptr) {
    return unavailable();
  }
  const std::optional<SourceRef> ref = parse_ref_param(ref_text);
  if (!ref.has_value()) {
    return bad_ref();
  }
  const std::optional<Aspect> aspect = parse_aspect(aspect_text);
  if (!aspect.has_value()) {
    return invalid("unknown aspect: " + aspect_text);
  }
  const Status status = resolver->clear_overrides(*ref, *aspect, context_of(req));
  if (!status.ok()) {
    return fail(status.error());
  }
  return http_server::textResponse(204, "");
}

// ---------------------------------------------------------------------------
// 播放会话（7.1–7.4）。字节只从会话出；locator 不出进程。
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<PlaybackService> playback_or_null() {
  return media_library::playback_service_ptr();
}

} // namespace

http_server::ResponsePtr playback_open(const http_server::RequestPtr &req,
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
  return http_util::jsonResponse(201, json_util::dump(session_json(session.value())));
}

http_server::ResponsePtr playback_session(const http_server::RequestPtr &,
                                const std::string &session_id) {
  const std::shared_ptr<PlaybackService> service = playback_or_null();
  if (service == nullptr) {
    return unavailable();
  }
  Result<PlaybackSession> session = service->session(session_id);
  if (!session.ok()) {
    return fail(session.error());
  }
  return http_util::jsonResponse(200, json_util::dump(session_json(session.value())));
}

http_server::ResponsePtr playback_refresh(const http_server::RequestPtr &req,
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
  return http_util::jsonResponse(200, json_util::dump(session_json(session.value())));
}

http_server::ResponsePtr playback_close(const http_server::RequestPtr &,
                              const std::string &session_id) {
  const std::shared_ptr<PlaybackService> service = playback_or_null();
  if (service == nullptr) {
    return unavailable();
  }
  Status status = service->close(session_id);
  if (!status.ok()) {
    return fail(status.error());
  }
  return http_server::textResponse(204, "");
}

http_server::ResponsePtr playback_stream(const http_server::RequestPtr &req,
                               const std::string &session_id) {
  return stream_http::serve_session(req, session_id, req && req->method() == drogon::Head);
}

} // namespace api_v1
