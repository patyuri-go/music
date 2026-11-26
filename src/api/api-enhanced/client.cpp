#include "api/api-enhanced/client.h"

#include <json/json.h>

#include "json_util.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace api_enhanced {
namespace {

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

media_library::Error invalid_json(const std::string &path) {
  return media_library::Error::make(
      media_library::ErrorCode::ProviderUnavailable,
      "api-enhanced 返回非法 JSON (" + path + ")");
}

media_library::Error platform_error(const std::string &path, std::int64_t code) {
  // §7.4：非 200 的平台码按平台错误处理，retryable 由调用方决定。
  if (code == 301) {
    media_library::Error error = media_library::Error::make(
        media_library::ErrorCode::AuthRequired, "netease 要求登录 (" + path + ")");
    error.restriction = media_library::RestrictionCode::AuthRequired;
    return error;
  }
  if (code == -460) {
    media_library::Error error = media_library::Error::make(
        media_library::ErrorCode::RateLimited, "netease 风控 (" + path + ")");
    error.retryable = true;
    error.retry_after = 60000;
    return error;
  }
  if (code == 404 || code == -447) {
    media_library::Error error = media_library::Error::make(
        media_library::ErrorCode::PermissionDenied, "netease 平台拒绝 (" + path + ")");
    error.restriction = media_library::RestrictionCode::RegionBlocked;
    return error;
  }
  media_library::Error error = media_library::Error::make(
      media_library::ErrorCode::ProviderUnavailable,
      "netease 平台错误 " + std::to_string(code) + " (" + path + ")");
  error.retryable = true;
  return error;
}

bool is_object(const Json::Value &value) {
  return !value.isNull() && value.isObject();
}

bool is_list(const Json::Value &value) {
  return !value.isNull() && value.isArray();
}

bool is_string(const Json::Value &value) {
  return !value.isNull() && value.isString();
}

bool is_number(const Json::Value &value) {
  return !value.isNull() && value.isNumeric();
}

// 缺键、空值、类型不符都退化为默认值：平台字段随时可能缺失，不能因为
// 少一个可选字段就让整次请求失败。
std::string field_string(const Json::Value &object, const char *key) {
  if (!is_object(object) || !object.isMember(key)) {
    return {};
  }
  const Json::Value &value = object[key];
  if (!is_string(value)) {
    return {};
  }
  return std::string(value.asString());
}

// 平台对同一字段可能给数字，也可能给字符串（cd 是最典型的一例）。
// jsoncpp 的 asInt64() 只接受数值类型，遇到字符串会抛
// "Value is not convertible to Int64."，所以字符串必须自己解析：
// from_chars 不抛异常、不受 locale 影响，非数字串与溢出都回退默认值。
std::int64_t parse_int64(const std::string &text, std::int64_t fallback) {
  if (text.empty()) {
    return fallback;
  }
  std::int64_t value = 0;
  const char *first = text.data();
  const char *last = first + text.size();
  const std::from_chars_result result = std::from_chars(first, last, value);
  if (result.ec != std::errc() || result.ptr != last) {
    return fallback;
  }
  return value;
}

std::int64_t field_int64(const Json::Value &object, const char *key,
                         std::int64_t fallback = 0) {
  if (!is_object(object) || !object.isMember(key)) {
    return fallback;
  }
  const Json::Value &value = object[key];
  if (is_number(value)) {
    return static_cast<std::int64_t>(value.asInt64());
  }
  if (is_string(value)) {
    return parse_int64(value.asString(), fallback);
  }
  return fallback;
}

bool field_bool(const Json::Value &object, const char *key,
                bool fallback = false) {
  if (!is_object(object) || !object.isMember(key)) {
    return fallback;
  }
  const Json::Value &value = object[key];
  if (value.isBool()) {
    return value.asBool();
  }
  return fallback;
}

std::vector<std::string> field_strings(const Json::Value &object,
                                       const char *key) {
  std::vector<std::string> out;
  if (!is_object(object) || !object.isMember(key)) {
    return out;
  }
  const Json::Value &value = object[key];
  if (!is_list(value)) {
    return out;
  }
  for (Json::ArrayIndex i = 0; i < value.size(); ++i) {
    const Json::Value &item = value[i];
    if (is_string(item)) {
      out.emplace_back(item.asString());
    }
  }
  return out;
}

std::vector<NeteaseArtistRef> parse_artists(const Json::Value &value) {
  std::vector<NeteaseArtistRef> out;
  if (!is_list(value)) {
    return out;
  }
  for (Json::ArrayIndex i = 0; i < value.size(); ++i) {
    const Json::Value &item = value[i];
    if (!is_object(item)) {
      continue;
    }
    NeteaseArtistRef artist;
    artist.id = field_int64(item, "id");
    artist.name = field_string(item, "name");
    if (artist.id <= 0) {
      continue;
    }
    out.push_back(std::move(artist));
  }
  return out;
}

NeteaseQuality parse_quality(const Json::Value &object,
                             const char *key) {
  NeteaseQuality quality;
  if (!is_object(object) || !object.isMember(key)) {
    return quality;
  }
  const Json::Value &value = object[key];
  if (!is_object(value)) {
    return quality;
  }
  quality.present = true;
  quality.br = field_int64(value, "br");
  quality.size = field_int64(value, "size");
  quality.sample_rate = field_int64(value, "sr");
  return quality;
}

// album 对象在 /album、/cloudsearch?type=10、/artist/album 三处的形状一致；
// 曲目里的 al{} 是它的真子集，同一个解析器够用。
NeteaseAlbumBrief parse_album_brief(const Json::Value &object) {
  NeteaseAlbumBrief album;
  if (!is_object(object)) {
    return album;
  }
  album.id = field_int64(object, "id");
  album.name = field_string(object, "name");
  album.pic_url = field_string(object, "picUrl");
  album.publish_time_ms = field_int64(object, "publishTime");
  album.tags = field_strings(object, "tags");
  album.description = field_string(object, "description");
  if (object.isMember("artists")) {
    album.artists = parse_artists(object["artists"]);
  }
  // artists[] 为空时回退单对象 artist（老版本/部分端点只给这一个）。
  if (album.artists.empty() && object.isMember("artist")) {
    const Json::Value &single = object["artist"];
    if (is_object(single)) {
      NeteaseArtistRef artist;
      artist.id = field_int64(single, "id");
      artist.name = field_string(single, "name");
      if (artist.id > 0) {
        album.artists.push_back(std::move(artist));
      }
    }
  }
  return album;
}

NeteaseSong parse_song(const Json::Value &object) {
  NeteaseSong song;
  if (!is_object(object)) {
    return song;
  }
  song.id = field_int64(object, "id");
  song.name = field_string(object, "name");
  song.duration_ms = field_int64(object, "dt");
  song.fee = static_cast<int>(field_int64(object, "fee"));
  song.no = static_cast<int>(field_int64(object, "no"));
  song.cd = static_cast<int>(field_int64(object, "cd"));
  song.publish_time_ms = field_int64(object, "publishTime");
  song.alias = field_strings(object, "alia");
  if (song.alias.empty()) {
    // cloudsearch 走另一套字段名。
    song.alias = field_strings(object, "alias");
  }
  song.translated_names = field_strings(object, "tns");
  if (song.translated_names.empty()) {
    song.translated_names = field_strings(object, "transNames");
  }
  if (object.isMember("ar")) {
    song.artists = parse_artists(object["ar"]);
  }
  if (object.isMember("al")) {
    song.album = parse_album_brief(object["al"]);
  }
  song.l = parse_quality(object, "l");
  song.m = parse_quality(object, "m");
  song.h = parse_quality(object, "h");
  song.sq = parse_quality(object, "sq");
  song.hr = parse_quality(object, "hr");
  return song;
}

NeteaseArtist parse_artist(const Json::Value &object) {
  NeteaseArtist artist;
  if (!is_object(object)) {
    return artist;
  }
  artist.id = field_int64(object, "id");
  artist.name = field_string(object, "name");
  artist.pic_url = field_string(object, "picUrl");
  artist.album_size = field_int64(object, "albumSize");
  artist.music_size = field_int64(object, "musicSize");
  artist.brief_desc = field_string(object, "briefDesc");
  return artist;
}

NeteasePlaylist parse_playlist(const Json::Value &object) {
  NeteasePlaylist playlist;
  if (!is_object(object)) {
    return playlist;
  }
  playlist.id = field_int64(object, "id");
  playlist.name = field_string(object, "name");
  playlist.cover_img_url = field_string(object, "coverImgUrl");
  playlist.track_count = field_int64(object, "trackCount");
  playlist.update_time_ms = field_int64(object, "updateTime");
  playlist.description = field_string(object, "description");
  playlist.tags = field_strings(object, "tags");
  if (object.isMember("creator")) {
    playlist.creator_name = field_string(object["creator"], "nickname");
  }
  return playlist;
}

// 统一入口：解析 body，校验 code，然后交给 extractor 取数据。
template <typename T, typename Extractor>
media_library::Result<T>
parse_body(const std::string &body, const std::string &path, Extractor extract) {
  const Json::Value json = json_util::parseOrNull(body);
  if (json.isNull() || !json.isObject()) {
    return media_library::Result<T>::failure(invalid_json(path));
  }
  if (json.isMember("code") && is_number(json["code"])) {
    const std::int64_t code = static_cast<std::int64_t>(json["code"].asInt64());
    if (code != 200) {
      return media_library::Result<T>::failure(platform_error(path, code));
    }
  }
  return media_library::Result<T>::success(extract(json));
}

std::string join_ids(const std::vector<std::int64_t> &ids) {
  std::string out;
  for (const std::int64_t id : ids) {
    if (!out.empty()) {
      out.push_back(',');
    }
    out.append(std::to_string(id));
  }
  return out;
}

} // namespace

NeteaseClient::NeteaseClient(std::shared_ptr<NeteaseTransport> transport,
                             NeteaseClientOptions options)
    : transport_(std::move(transport)), options_(std::move(options)) {}

media_library::Result<std::string>
NeteaseClient::get_json(const std::string &path,
                        std::vector<std::pair<std::string, std::string>> query,
                        const media_library::CallContext &ctx,
                        bool cacheable) {
  if (transport_ == nullptr) {
    media_library::Error error = media_library::Error::make(
        media_library::ErrorCode::ProviderUnavailable,
        "netease transport 未初始化");
    error.retryable = true;
    return media_library::Result<std::string>::failure(std::move(error));
  }

  // 凭据与 realIP 由本层注入，transport 只管拼 URL 发请求。
  if (!options_.cookie.empty()) {
    query.emplace_back("cookie", options_.cookie);
  }
  if (!options_.real_ip.empty()) {
    query.emplace_back("realIP", options_.real_ip);
  }

  std::string cache_key;
  if (cacheable) {
    cache_key = path;
    for (const auto &item : query) {
      cache_key.push_back('\n');
      cache_key.append(item.first);
      cache_key.push_back('=');
      cache_key.append(item.second);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cache_.find(cache_key);
    if (it != cache_.end() && it->second.expires_at_ms > now_ms()) {
      return media_library::Result<std::string>::success(it->second.body);
    }
  }

  auto fetched = transport_->get(path, query, ctx);
  if (!fetched.ok()) {
    return fetched;
  }

  if (cacheable && options_.metadata_ttl_ms > 0) {
    const std::int64_t expires = now_ms() + options_.metadata_ttl_ms;
    std::lock_guard<std::mutex> lock(mutex_);
    // 容量上限用最朴素的「满了清空」——缓存只是省流量，不需要 LRU 精度。
    if (options_.cache_capacity > 0 &&
        static_cast<int>(cache_.size()) >= options_.cache_capacity) {
      cache_.clear();
    }
    cache_[std::move(cache_key)] = CacheEntry{fetched.value(), expires};
  }
  return fetched;
}

media_library::Result<NeteaseLoginStatus>
NeteaseClient::login_status(const media_library::CallContext &ctx) {
  auto body = get_json("/login/status", {}, ctx, /*cacheable=*/false);
  if (!body.ok()) {
    return media_library::Result<NeteaseLoginStatus>::failure(body.error());
  }

  const Json::Value json = json_util::parseOrNull(body.value());
  if (json.isNull() || !json.isObject()) {
    return media_library::Result<NeteaseLoginStatus>::failure(
        invalid_json("/login/status"));
  }

  NeteaseLoginStatus status;
  // 该端点的语义是 {"data":{"code":200,"account":...}}；
  // 兼容极老版本直接给顶层 account。
  const Json::Value &payload =
      json.isMember("data") && json["data"].isObject()
          ? json["data"]
          : json;

  if (payload.isMember("account") &&
      !payload["account"].isNull() &&
      payload["account"].isObject()) {
    const Json::Value &account = payload["account"];
    status.authenticated = true;
    if (account.isMember("id") && account["id"].isNumeric()) {
      status.user_id = static_cast<std::int64_t>(account["id"].asInt64());
    }
    if (account.isMember("vipType") &&
        account["vipType"].isNumeric()) {
      status.vip_type = static_cast<int>(account["vipType"].asInt64());
    }
  }

  if (payload.isMember("profile") &&
      payload["profile"].isObject()) {
    const Json::Value &profile = payload["profile"];
    if (profile.isMember("nickname") &&
        profile["nickname"].isString()) {
      status.nickname = profile["nickname"].asString();
    }
  }

  return media_library::Result<NeteaseLoginStatus>::success(std::move(status));
}

media_library::Result<std::vector<NeteaseSong>>
NeteaseClient::song_detail(const std::vector<std::int64_t> &ids,
                           const media_library::CallContext &ctx) {
  using Result = media_library::Result<std::vector<NeteaseSong>>;
  if (ids.empty()) {
    return Result::success({});
  }
  auto body = get_json("/song/detail", {{"ids", join_ids(ids)}}, ctx);
  if (!body.ok()) {
    return Result::failure(body.error());
  }
  return parse_body<std::vector<NeteaseSong>>(
      body.value(), "/song/detail", [](const Json::Value &json) {
        std::vector<NeteaseSong> songs;
        if (!json.isMember("songs") || !is_list(json["songs"])) {
          return songs;
        }
        const Json::Value &list = json["songs"];
        songs.reserve(list.size());
        for (Json::ArrayIndex i = 0; i < list.size(); ++i) {
          NeteaseSong song = parse_song(list[i]);
          if (song.id > 0) {
            songs.push_back(std::move(song));
          }
        }
        return songs;
      });
}

media_library::Result<NeteaseAlbum>
NeteaseClient::album(std::int64_t id, const media_library::CallContext &ctx) {
  using Result = media_library::Result<NeteaseAlbum>;
  if (id <= 0) {
    return Result::failure(media_library::Error::make(
        media_library::ErrorCode::InvalidRequest, "netease album id is invalid"));
  }
  auto body = get_json("/album", {{"id", std::to_string(id)}}, ctx);
  if (!body.ok()) {
    return Result::failure(body.error());
  }
  return parse_body<NeteaseAlbum>(
      body.value(), "/album", [](const Json::Value &json) {
        NeteaseAlbum album;
        if (json.isMember("album")) {
          album.brief = parse_album_brief(json["album"]);
        }
        // 实测曲目在顶层 songs[]，album.songs 为空数组。
        if (json.isMember("songs") && is_list(json["songs"])) {
          const Json::Value &list = json["songs"];
          album.songs.reserve(list.size());
          for (Json::ArrayIndex i = 0; i < list.size(); ++i) {
            NeteaseSong song = parse_song(list[i]);
            if (song.id > 0) {
              album.songs.push_back(std::move(song));
            }
          }
        }
        return album;
      });
}

media_library::Result<NeteaseArtist>
NeteaseClient::artist(std::int64_t id, const media_library::CallContext &ctx) {
  using Result = media_library::Result<NeteaseArtist>;
  if (id <= 0) {
    return Result::failure(media_library::Error::make(
        media_library::ErrorCode::InvalidRequest,
        "netease artist id is invalid"));
  }
  auto body = get_json("/artists", {{"id", std::to_string(id)}}, ctx);
  if (!body.ok()) {
    return Result::failure(body.error());
  }
  return parse_body<NeteaseArtist>(
      body.value(), "/artists", [](const Json::Value &json) {
        NeteaseArtist artist;
        if (json.isMember("artist")) {
          artist = parse_artist(json["artist"]);
        }
        return artist;
      });
}

media_library::Result<NeteasePage<NeteaseAlbumBrief>>
NeteaseClient::artist_albums(std::int64_t id, int limit, int offset,
                             const media_library::CallContext &ctx) {
  using Result = media_library::Result<NeteasePage<NeteaseAlbumBrief>>;
  if (id <= 0) {
    return Result::failure(media_library::Error::make(
        media_library::ErrorCode::InvalidRequest,
        "netease artist id is invalid"));
  }
  auto body = get_json("/artist/album",
                       {{"id", std::to_string(id)},
                        {"limit", std::to_string(limit)},
                        {"offset", std::to_string(offset)}},
                       ctx);
  if (!body.ok()) {
    return Result::failure(body.error());
  }
  return parse_body<NeteasePage<NeteaseAlbumBrief>>(
      body.value(), "/artist/album", [](const Json::Value &json) {
        NeteasePage<NeteaseAlbumBrief> page;
        page.has_more = field_bool(json, "more");
        if (json.isMember("hotAlbums") && is_list(json["hotAlbums"])) {
          const Json::Value &list = json["hotAlbums"];
          page.items.reserve(list.size());
          for (Json::ArrayIndex i = 0; i < list.size(); ++i) {
            NeteaseAlbumBrief album = parse_album_brief(list[i]);
            if (album.id > 0) {
              page.items.push_back(std::move(album));
            }
          }
        }
        return page;
      });
}

media_library::Result<NeteasePage<NeteaseSong>>
NeteaseClient::search_songs(const std::string &keywords, int limit, int offset,
                            const media_library::CallContext &ctx) {
  using Result = media_library::Result<NeteasePage<NeteaseSong>>;
  if (keywords.empty()) {
    return Result::success({});
  }
  auto body = get_json("/cloudsearch",
                       {{"keywords", keywords},
                        {"type", "1"},
                        {"limit", std::to_string(limit)},
                        {"offset", std::to_string(offset)}},
                       ctx);
  if (!body.ok()) {
    return Result::failure(body.error());
  }
  return parse_body<NeteasePage<NeteaseSong>>(
      body.value(), "/cloudsearch", [offset](const Json::Value &json) {
        NeteasePage<NeteaseSong> page;
        if (!json.isMember("result") || !is_object(json["result"])) {
          return page;
        }
        const Json::Value &result = json["result"];
        if (!result.isMember("songs") || !is_list(result["songs"])) {
          return page;
        }
        const Json::Value &list = result["songs"];
        page.items.reserve(list.size());
        for (Json::ArrayIndex i = 0; i < list.size(); ++i) {
          NeteaseSong song = parse_song(list[i]);
          if (song.id > 0) {
            page.items.push_back(std::move(song));
          }
        }
        // cloudsearch 不直接给 more；用 songCount 与 offset+返回数比较。
        const std::int64_t count = field_int64(result, "songCount");
        page.has_more =
            count > 0 && static_cast<std::int64_t>(offset) +
                                  static_cast<std::int64_t>(page.items.size()) <
                              count;
        return page;
      });
}

media_library::Result<NeteasePage<NeteaseAlbumBrief>>
NeteaseClient::search_albums(const std::string &keywords, int limit,
                             int offset, const media_library::CallContext &ctx) {
  using Result = media_library::Result<NeteasePage<NeteaseAlbumBrief>>;
  if (keywords.empty()) {
    return Result::success({});
  }
  auto body = get_json("/cloudsearch",
                       {{"keywords", keywords},
                        {"type", "10"},
                        {"limit", std::to_string(limit)},
                        {"offset", std::to_string(offset)}},
                       ctx);
  if (!body.ok()) {
    return Result::failure(body.error());
  }
  return parse_body<NeteasePage<NeteaseAlbumBrief>>(
      body.value(), "/cloudsearch", [offset](const Json::Value &json) {
        NeteasePage<NeteaseAlbumBrief> page;
        if (!json.isMember("result") || !is_object(json["result"])) {
          return page;
        }
        const Json::Value &result = json["result"];
        if (!result.isMember("albums") || !is_list(result["albums"])) {
          return page;
        }
        const Json::Value &list = result["albums"];
        page.items.reserve(list.size());
        for (Json::ArrayIndex i = 0; i < list.size(); ++i) {
          NeteaseAlbumBrief album = parse_album_brief(list[i]);
          if (album.id > 0) {
            page.items.push_back(std::move(album));
          }
        }
        const std::int64_t count = field_int64(result, "albumCount");
        page.has_more =
            count > 0 && static_cast<std::int64_t>(offset) +
                                  static_cast<std::int64_t>(page.items.size()) <
                              count;
        return page;
      });
}

media_library::Result<NeteasePage<NeteaseArtist>>
NeteaseClient::search_artists(const std::string &keywords, int limit,
                              int offset, const media_library::CallContext &ctx) {
  using Result = media_library::Result<NeteasePage<NeteaseArtist>>;
  if (keywords.empty()) {
    return Result::success({});
  }
  auto body = get_json("/cloudsearch",
                       {{"keywords", keywords},
                        {"type", "100"},
                        {"limit", std::to_string(limit)},
                        {"offset", std::to_string(offset)}},
                       ctx);
  if (!body.ok()) {
    return Result::failure(body.error());
  }
  return parse_body<NeteasePage<NeteaseArtist>>(
      body.value(), "/cloudsearch", [offset](const Json::Value &json) {
        NeteasePage<NeteaseArtist> page;
        if (!json.isMember("result") || !is_object(json["result"])) {
          return page;
        }
        const Json::Value &result = json["result"];
        if (!result.isMember("artists") || !is_list(result["artists"])) {
          return page;
        }
        const Json::Value &list = result["artists"];
        page.items.reserve(list.size());
        for (Json::ArrayIndex i = 0; i < list.size(); ++i) {
          NeteaseArtist artist = parse_artist(list[i]);
          if (artist.id > 0) {
            page.items.push_back(std::move(artist));
          }
        }
        const std::int64_t count = field_int64(result, "artistCount");
        page.has_more =
            count > 0 && static_cast<std::int64_t>(offset) +
                                  static_cast<std::int64_t>(page.items.size()) <
                              count;
        return page;
      });
}

media_library::Result<NeteaseSongUrl>
NeteaseClient::song_url(std::int64_t id, const std::string &level,
                        const media_library::CallContext &ctx) {
  using Result = media_library::Result<NeteaseSongUrl>;
  if (id <= 0) {
    return Result::failure(media_library::Error::make(
        media_library::ErrorCode::InvalidRequest, "netease song id is invalid"));
  }
  // 直链短时有效且每次都要新 token：永不缓存。
  auto body = get_json("/song/url/v1",
                       {{"id", std::to_string(id)}, {"level", level}}, ctx,
                       /*cacheable=*/false);
  if (!body.ok()) {
    return Result::failure(body.error());
  }
  return parse_body<NeteaseSongUrl>(
      body.value(), "/song/url/v1", [](const Json::Value &json) {
        NeteaseSongUrl url;
        if (!json.isMember("data") || !is_list(json["data"])) {
          return url;
        }
        const Json::Value &list = json["data"];
        if (list.size() == 0 || !is_object(list[0])) {
          return url;
        }
        const Json::Value &item = list[0];
        url.url = field_string(item, "url");
        url.type = field_string(item, "type");
        url.bitrate = field_int64(item, "br");
        url.size = field_int64(item, "size");
        url.sample_rate = field_int64(item, "sr");
        url.level_name = field_string(item, "level");
        url.expi_seconds = field_int64(item, "expi");
        url.fee = static_cast<int>(field_int64(item, "fee"));
        url.code = static_cast<int>(field_int64(item, "code"));
        url.time_ms = field_int64(item, "time");
        if (item.isMember("freeTrialInfo") && is_object(item["freeTrialInfo"])) {
          const Json::Value &trial = item["freeTrialInfo"];
          url.free_trial = true;
          url.trial_end_seconds = field_int64(trial, "end");
        } else if (item.isMember("freeTrialPrivilege") &&
                   is_object(item["freeTrialPrivilege"])) {
          const Json::Value &trial = item["freeTrialPrivilege"];
          // resConsumable=false 且不可重复试听时也算试听限制。
          if (field_bool(trial, "resConsumable", true) == false) {
            url.free_trial = true;
          }
        }
        return url;
      });
}

media_library::Result<NeteaseLyrics>
NeteaseClient::lyric(std::int64_t id, const media_library::CallContext &ctx) {
  using Result = media_library::Result<NeteaseLyrics>;
  if (id <= 0) {
    return Result::failure(media_library::Error::make(
        media_library::ErrorCode::InvalidRequest, "netease lyric id is invalid"));
  }
  auto body = get_json("/lyric/new", {{"id", std::to_string(id)}}, ctx);
  if (!body.ok()) {
    return Result::failure(body.error());
  }
  return parse_body<NeteaseLyrics>(
      body.value(), "/lyric/new", [](const Json::Value &json) {
        NeteaseLyrics lyrics;
        std::string lrc_text;
        if (json.isMember("lrc") && is_object(json["lrc"])) {
          lrc_text = field_string(json["lrc"], "lyric");
        }
        if (json.isMember("tlyric") && is_object(json["tlyric"])) {
          lyrics.translation = field_string(json["tlyric"], "lyric");
        }
        if (json.isMember("romalrc") && is_object(json["romalrc"])) {
          lyrics.romanization = field_string(json["romalrc"], "lyric");
        }
        if (json.isMember("yrc") && is_object(json["yrc"])) {
          lyrics.body = field_string(json["yrc"], "lyric");
        }
        if (lyrics.body.empty()) {
          lyrics.body = lrc_text;
        }
        // ytlrc 是逐字译文，比行级 tlyric 更准：有就优先用它。
        if (json.isMember("ytlrc") && is_object(json["ytlrc"])) {
          const std::string ytlrc = field_string(json["ytlrc"], "lyric");
          if (!ytlrc.empty()) {
            lyrics.translation = ytlrc;
          }
        }
        lyrics.pure_music = field_bool(json, "pureMusic", false);
        return lyrics;
      });
}

media_library::Result<NeteasePage<NeteasePlaylist>>
NeteaseClient::user_playlists(const std::string &uid, int limit, int offset,
                              const media_library::CallContext &ctx) {
  using Result = media_library::Result<NeteasePage<NeteasePlaylist>>;
  if (uid.empty()) {
    return Result::failure(media_library::Error::make(
        media_library::ErrorCode::InvalidRequest, "netease uid is required"));
  }
  auto body = get_json("/user/playlist",
                       {{"uid", uid},
                        {"limit", std::to_string(limit)},
                        {"offset", std::to_string(offset)}},
                       ctx);
  if (!body.ok()) {
    return Result::failure(body.error());
  }
  return parse_body<NeteasePage<NeteasePlaylist>>(
      body.value(), "/user/playlist", [](const Json::Value &json) {
        NeteasePage<NeteasePlaylist> page;
        if (!json.isMember("playlist") || !is_list(json["playlist"])) {
          return page;
        }
        const Json::Value &list = json["playlist"];
        page.items.reserve(list.size());
        for (Json::ArrayIndex i = 0; i < list.size(); ++i) {
          NeteasePlaylist playlist = parse_playlist(list[i]);
          if (playlist.id > 0) {
            page.items.push_back(std::move(playlist));
          }
        }
        page.has_more = field_bool(json, "more", false);
        return page;
      });
}

media_library::Result<NeteasePlaylist>
NeteaseClient::playlist(std::int64_t id, const media_library::CallContext &ctx) {
  using Result = media_library::Result<NeteasePlaylist>;
  if (id <= 0) {
    return Result::failure(media_library::Error::make(
        media_library::ErrorCode::InvalidRequest,
        "netease playlist id is invalid"));
  }
  auto body =
      get_json("/playlist/detail", {{"id", std::to_string(id)}}, ctx);
  if (!body.ok()) {
    return Result::failure(body.error());
  }
  return parse_body<NeteasePlaylist>(
      body.value(), "/playlist/detail", [](const Json::Value &json) {
        if (!json.isMember("playlist") || !is_object(json["playlist"])) {
          return NeteasePlaylist{};
        }
        return parse_playlist(json["playlist"]);
      });
}

media_library::Result<NeteasePage<NeteaseSong>>
NeteaseClient::playlist_tracks(std::int64_t id, int limit, int offset,
                               const media_library::CallContext &ctx) {
  using Result = media_library::Result<NeteasePage<NeteaseSong>>;
  if (id <= 0) {
    return Result::failure(media_library::Error::make(
        media_library::ErrorCode::InvalidRequest,
        "netease playlist id is invalid"));
  }
  auto body = get_json("/playlist/track/all",
                       {{"id", std::to_string(id)},
                        {"limit", std::to_string(limit)},
                        {"offset", std::to_string(offset)}},
                       ctx);
  if (!body.ok()) {
    return Result::failure(body.error());
  }
  return parse_body<NeteasePage<NeteaseSong>>(
      body.value(), "/playlist/track/all", [limit](const Json::Value &json) {
        NeteasePage<NeteaseSong> page;
        if (!json.isMember("songs") || !is_list(json["songs"])) {
          return page;
        }
        const Json::Value &list = json["songs"];
        page.items.reserve(list.size());
        for (Json::ArrayIndex i = 0; i < list.size(); ++i) {
          NeteaseSong song = parse_song(list[i]);
          if (song.id > 0) {
            page.items.push_back(std::move(song));
          }
        }
        // 平台不返回 more：拿满一页就认为还有下一批。
        page.has_more =
            limit > 0 && static_cast<int>(page.items.size()) >= limit;
        return page;
      });
}

} // namespace api_enhanced
