#include "api/api-enhanced/client.h"

#include "crow/json.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
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

bool is_object(const crow::json::rvalue &value) {
  return static_cast<bool>(value) && value.t() == crow::json::type::Object;
}

bool is_list(const crow::json::rvalue &value) {
  return static_cast<bool>(value) && value.t() == crow::json::type::List;
}

bool is_string(const crow::json::rvalue &value) {
  return static_cast<bool>(value) && value.t() == crow::json::type::String;
}

bool is_number(const crow::json::rvalue &value) {
  return static_cast<bool>(value) && value.t() == crow::json::type::Number;
}

// 缺键、空值、类型不符都退化为默认值：平台字段随时可能缺失，不能因为
// 少一个可选字段就让整次请求失败。
std::string field_string(const crow::json::rvalue &object, const char *key) {
  if (!is_object(object) || !object.has(key)) {
    return {};
  }
  const crow::json::rvalue &value = object[key];
  if (!is_string(value)) {
    return {};
  }
  return std::string(value.s());
}

// cd 可能是字符串 "1"，i() 同时接受 Number 与 String。
std::int64_t field_int64(const crow::json::rvalue &object, const char *key,
                         std::int64_t fallback = 0) {
  if (!is_object(object) || !object.has(key)) {
    return fallback;
  }
  const crow::json::rvalue &value = object[key];
  if (!is_number(value) && !is_string(value)) {
    return fallback;
  }
  return static_cast<std::int64_t>(value.i());
}

bool field_bool(const crow::json::rvalue &object, const char *key,
                bool fallback = false) {
  if (!is_object(object) || !object.has(key)) {
    return fallback;
  }
  const crow::json::rvalue &value = object[key];
  if (value.t() == crow::json::type::True) {
    return true;
  }
  if (value.t() == crow::json::type::False) {
    return false;
  }
  return fallback;
}

std::vector<std::string> field_strings(const crow::json::rvalue &object,
                                       const char *key) {
  std::vector<std::string> out;
  if (!is_object(object) || !object.has(key)) {
    return out;
  }
  const crow::json::rvalue &value = object[key];
  if (!is_list(value)) {
    return out;
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    const crow::json::rvalue &item = value[i];
    if (is_string(item)) {
      out.emplace_back(item.s());
    }
  }
  return out;
}

std::vector<NeteaseArtistRef> parse_artists(const crow::json::rvalue &value) {
  std::vector<NeteaseArtistRef> out;
  if (!is_list(value)) {
    return out;
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    const crow::json::rvalue &item = value[i];
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

NeteaseQuality parse_quality(const crow::json::rvalue &object,
                             const char *key) {
  NeteaseQuality quality;
  if (!is_object(object) || !object.has(key)) {
    return quality;
  }
  const crow::json::rvalue &value = object[key];
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
NeteaseAlbumBrief parse_album_brief(const crow::json::rvalue &object) {
  NeteaseAlbumBrief album;
  if (!is_object(object)) {
    return album;
  }
  album.id = field_int64(object, "id");
  album.name = field_string(object, "name");
  album.pic_url = field_string(object, "picUrl");
  album.publish_time_ms = field_int64(object, "publishTime");
  album.tags = field_strings(object, "tags");
  if (object.has("artists")) {
    album.artists = parse_artists(object["artists"]);
  }
  // artists[] 为空时回退单对象 artist（老版本/部分端点只给这一个）。
  if (album.artists.empty() && object.has("artist")) {
    const crow::json::rvalue &single = object["artist"];
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

NeteaseSong parse_song(const crow::json::rvalue &object) {
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
  if (object.has("ar")) {
    song.artists = parse_artists(object["ar"]);
  }
  if (object.has("al")) {
    song.album = parse_album_brief(object["al"]);
  }
  song.l = parse_quality(object, "l");
  song.m = parse_quality(object, "m");
  song.h = parse_quality(object, "h");
  song.sq = parse_quality(object, "sq");
  song.hr = parse_quality(object, "hr");
  return song;
}

NeteaseArtist parse_artist(const crow::json::rvalue &object) {
  NeteaseArtist artist;
  if (!is_object(object)) {
    return artist;
  }
  artist.id = field_int64(object, "id");
  artist.name = field_string(object, "name");
  artist.pic_url = field_string(object, "picUrl");
  artist.album_size = field_int64(object, "albumSize");
  artist.music_size = field_int64(object, "musicSize");
  return artist;
}

// 统一入口：解析 body，校验 code，然后交给 extractor 取数据。
template <typename T, typename Extractor>
media_library::Result<T>
parse_body(const std::string &body, const std::string &path, Extractor extract) {
  const crow::json::rvalue json = crow::json::load(body);
  if (!json || json.t() != crow::json::type::Object) {
    return media_library::Result<T>::failure(invalid_json(path));
  }
  if (json.has("code") && is_number(json["code"])) {
    const std::int64_t code = static_cast<std::int64_t>(json["code"].i());
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

  const crow::json::rvalue json = crow::json::load(body.value());
  if (!json || json.t() != crow::json::type::Object) {
    return media_library::Result<NeteaseLoginStatus>::failure(
        invalid_json("/login/status"));
  }

  NeteaseLoginStatus status;
  // 该端点的语义是 {"data":{"code":200,"account":...}}；
  // 兼容极老版本直接给顶层 account。
  const crow::json::rvalue &payload =
      json.has("data") && json["data"].t() == crow::json::type::Object
          ? json["data"]
          : json;

  if (payload.has("account") &&
      payload["account"].t() != crow::json::type::Null &&
      payload["account"].t() == crow::json::type::Object) {
    const crow::json::rvalue &account = payload["account"];
    status.authenticated = true;
    if (account.has("id") && account["id"].t() == crow::json::type::Number) {
      status.user_id = static_cast<std::int64_t>(account["id"].i());
    }
    if (account.has("vipType") &&
        account["vipType"].t() == crow::json::type::Number) {
      status.vip_type = static_cast<int>(account["vipType"].i());
    }
  }

  if (payload.has("profile") &&
      payload["profile"].t() == crow::json::type::Object) {
    const crow::json::rvalue &profile = payload["profile"];
    if (profile.has("nickname") &&
        profile["nickname"].t() == crow::json::type::String) {
      status.nickname = profile["nickname"].s();
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
      body.value(), "/song/detail", [](const crow::json::rvalue &json) {
        std::vector<NeteaseSong> songs;
        if (!json.has("songs") || !is_list(json["songs"])) {
          return songs;
        }
        const crow::json::rvalue &list = json["songs"];
        songs.reserve(list.size());
        for (std::size_t i = 0; i < list.size(); ++i) {
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
      body.value(), "/album", [](const crow::json::rvalue &json) {
        NeteaseAlbum album;
        if (json.has("album")) {
          album.brief = parse_album_brief(json["album"]);
        }
        // 实测曲目在顶层 songs[]，album.songs 为空数组。
        if (json.has("songs") && is_list(json["songs"])) {
          const crow::json::rvalue &list = json["songs"];
          album.songs.reserve(list.size());
          for (std::size_t i = 0; i < list.size(); ++i) {
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
      body.value(), "/artists", [](const crow::json::rvalue &json) {
        NeteaseArtist artist;
        if (json.has("artist")) {
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
      body.value(), "/artist/album", [](const crow::json::rvalue &json) {
        NeteasePage<NeteaseAlbumBrief> page;
        page.has_more = field_bool(json, "more");
        if (json.has("hotAlbums") && is_list(json["hotAlbums"])) {
          const crow::json::rvalue &list = json["hotAlbums"];
          page.items.reserve(list.size());
          for (std::size_t i = 0; i < list.size(); ++i) {
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
      body.value(), "/cloudsearch", [offset](const crow::json::rvalue &json) {
        NeteasePage<NeteaseSong> page;
        if (!json.has("result") || !is_object(json["result"])) {
          return page;
        }
        const crow::json::rvalue &result = json["result"];
        if (!result.has("songs") || !is_list(result["songs"])) {
          return page;
        }
        const crow::json::rvalue &list = result["songs"];
        page.items.reserve(list.size());
        for (std::size_t i = 0; i < list.size(); ++i) {
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
      body.value(), "/cloudsearch", [offset](const crow::json::rvalue &json) {
        NeteasePage<NeteaseAlbumBrief> page;
        if (!json.has("result") || !is_object(json["result"])) {
          return page;
        }
        const crow::json::rvalue &result = json["result"];
        if (!result.has("albums") || !is_list(result["albums"])) {
          return page;
        }
        const crow::json::rvalue &list = result["albums"];
        page.items.reserve(list.size());
        for (std::size_t i = 0; i < list.size(); ++i) {
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
      body.value(), "/cloudsearch", [offset](const crow::json::rvalue &json) {
        NeteasePage<NeteaseArtist> page;
        if (!json.has("result") || !is_object(json["result"])) {
          return page;
        }
        const crow::json::rvalue &result = json["result"];
        if (!result.has("artists") || !is_list(result["artists"])) {
          return page;
        }
        const crow::json::rvalue &list = result["artists"];
        page.items.reserve(list.size());
        for (std::size_t i = 0; i < list.size(); ++i) {
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
      body.value(), "/song/url/v1", [](const crow::json::rvalue &json) {
        NeteaseSongUrl url;
        if (!json.has("data") || !is_list(json["data"])) {
          return url;
        }
        const crow::json::rvalue &list = json["data"];
        if (list.size() == 0 || !is_object(list[0])) {
          return url;
        }
        const crow::json::rvalue &item = list[0];
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
        if (item.has("freeTrialInfo") && is_object(item["freeTrialInfo"])) {
          const crow::json::rvalue &trial = item["freeTrialInfo"];
          url.free_trial = true;
          url.trial_end_seconds = field_int64(trial, "end");
        } else if (item.has("freeTrialPrivilege") &&
                   is_object(item["freeTrialPrivilege"])) {
          const crow::json::rvalue &trial = item["freeTrialPrivilege"];
          // resConsumable=false 且不可重复试听时也算试听限制。
          if (field_bool(trial, "resConsumable", true) == false) {
            url.free_trial = true;
          }
        }
        return url;
      });
}

namespace {

std::string trim_cr(std::string line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.pop_back();
  }
  return line;
}

bool parse_clock(const std::string &text, std::int64_t &out_ms) {
  // "mm:ss" / "mm:ss.xx" / "mm:ss.xxx"
  const std::size_t colon = text.find(':');
  if (colon == std::string::npos || colon == 0) {
    return false;
  }
  const std::string minutes = text.substr(0, colon);
  const std::string rest = text.substr(colon + 1);
  const std::size_t dot = rest.find('.');
  const std::string seconds = dot == std::string::npos ? rest : rest.substr(0, dot);
  const std::string fraction =
      dot == std::string::npos ? std::string() : rest.substr(dot + 1);
  if (minutes.empty() || seconds.empty()) {
    return false;
  }
  for (char c : minutes) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  for (char c : seconds) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  std::int64_t frac_ms = 0;
  if (!fraction.empty()) {
    for (char c : fraction) {
      if (c < '0' || c > '9') {
        return false;
      }
    }
    std::string digits = fraction;
    if (digits.size() > 3) {
      digits = digits.substr(0, 3);
    }
    while (digits.size() < 3) {
      digits.push_back('0');
    }
    frac_ms = std::stoll(digits);
  }
  out_ms = std::stoll(minutes) * 60000 + std::stoll(seconds) * 1000 + frac_ms;
  return true;
}

// 取一行开头的所有 [ ... ] 标签；第一个可解析的时间标签作为行时间，
// 最后一个标签之后的内容作为正文。非 '[' 开头的行（例如平台混进来的
// JSON 人员行）直接跳过。
bool parse_lrc_line(const std::string &raw, std::int64_t &start_ms,
                    std::string &text, std::int64_t &offset_ms) {
  if (raw.empty() || raw.front() != '[') {
    return false;
  }
  std::size_t pos = 0;
  bool have_time = false;
  while (pos < raw.size() && raw[pos] == '[') {
    const std::size_t close = raw.find(']', pos);
    if (close == std::string::npos) {
      return false;
    }
    const std::string tag = raw.substr(pos + 1, close - pos - 1);
    if (tag.rfind("offset:", 0) == 0) {
      try {
        offset_ms = std::stoll(tag.substr(7));
      } catch (...) {
        offset_ms = 0;
      }
    } else if (!have_time) {
      std::int64_t value = 0;
      if (parse_clock(tag, value)) {
        start_ms = value;
        have_time = true;
      }
    }
    pos = close + 1;
  }
  if (!have_time) {
    return false;
  }
  text = raw.substr(pos);
  return true;
}

std::vector<std::pair<std::int64_t, std::string>>
parse_lrc(const std::string &body) {
  std::vector<std::pair<std::int64_t, std::string>> lines;
  std::int64_t offset_ms = 0;
  std::size_t begin = 0;
  while (begin <= body.size()) {
    const std::size_t end = body.find('\n', begin);
    const std::string raw = trim_cr(
        body.substr(begin, end == std::string::npos ? std::string::npos
                                                    : end - begin));
    std::int64_t start_ms = 0;
    std::string text;
    if (parse_lrc_line(raw, start_ms, text, offset_ms)) {
      lines.emplace_back(start_ms, std::move(text));
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  if (offset_ms != 0) {
    for (auto &line : lines) {
      line.first = std::max<std::int64_t>(0, line.first + offset_ms);
    }
  }
  return lines;
}

// YRC：[lineStart,lineDur](wordStart,wordDur,0)word...
// 实测 wordStart 是绝对毫秒；若出现相对值（< lineStart）则按相对处理。
std::vector<NeteaseLyricLine> parse_yrc(const std::string &body) {
  std::vector<NeteaseLyricLine> lines;
  std::size_t begin = 0;
  while (begin <= body.size()) {
    const std::size_t end = body.find('\n', begin);
    const std::string raw = trim_cr(
        body.substr(begin, end == std::string::npos ? std::string::npos
                                                    : end - begin));
    if (!raw.empty() && raw.front() == '[') {
      const std::size_t close = raw.find(']');
      if (close != std::string::npos) {
        const std::string head = raw.substr(1, close - 1);
        const std::size_t comma = head.find(',');
        if (comma != std::string::npos) {
          try {
            NeteaseLyricLine line;
            line.start_ms = std::stoll(head.substr(0, comma));
            const std::int64_t duration = std::stoll(head.substr(comma + 1));
            line.end_ms = line.start_ms + duration;
            std::size_t pos = close + 1;
            while (pos < raw.size() && raw[pos] == '(') {
              const std::size_t paren = raw.find(')', pos);
              if (paren == std::string::npos) {
                break;
              }
              const std::string word_head = raw.substr(pos + 1, paren - pos - 1);
              std::int64_t word_start = 0;
              std::int64_t word_duration = 0;
              bool parsed = false;
              const std::size_t c1 = word_head.find(',');
              if (c1 != std::string::npos) {
                const std::size_t c2 = word_head.find(',', c1 + 1);
                if (c2 != std::string::npos) {
                  try {
                    word_start = std::stoll(word_head.substr(0, c1));
                    word_duration = std::stoll(word_head.substr(c1 + 1, c2 - c1 - 1));
                    parsed = true;
                  } catch (...) {
                    parsed = false;
                  }
                }
              }
              const std::size_t next = raw.find('(', paren + 1);
              const std::string word_text =
                  raw.substr(paren + 1, next == std::string::npos
                                            ? std::string::npos
                                            : next - paren - 1);
              if (parsed) {
                if (word_start < line.start_ms) {
                  word_start += line.start_ms;
                }
                NeteaseLyricWord word;
                word.start_ms = word_start;
                if (word_duration > 0) {
                  word.end_ms = word_start + word_duration;
                }
                word.text = word_text;
                line.words.push_back(std::move(word));
              }
              line.text += word_text;
              pos = next == std::string::npos ? raw.size() : next;
            }
            lines.push_back(std::move(line));
          } catch (...) {
            // 单个坏行不影响整首歌词。
          }
        }
      }
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return lines;
}

} // namespace

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
      body.value(), "/lyric/new", [](const crow::json::rvalue &json) {
        NeteaseLyrics lyrics;
        std::string lrc_text;
        std::string tlyric_text;
        std::string yrc_text;
        if (json.has("lrc") && is_object(json["lrc"])) {
          lrc_text = field_string(json["lrc"], "lyric");
        }
        if (json.has("tlyric") && is_object(json["tlyric"])) {
          tlyric_text = field_string(json["tlyric"], "lyric");
        }
        if (json.has("yrc") && is_object(json["yrc"])) {
          yrc_text = field_string(json["yrc"], "lyric");
        }

        // 翻译按行时间对齐后并入正文；空译文不合并。
        std::vector<std::pair<std::int64_t, std::string>> translated =
            parse_lrc(tlyric_text);

        auto lines = parse_lrc(lrc_text);
        if (!lines.empty()) {
          lyrics.line_synced = true;
        }

        for (const auto &line : lines) {
          NeteaseLyricLine out;
          out.start_ms = line.first;
          out.text = line.second;
          if (!out.text.empty()) {
            for (const auto &item : translated) {
              if (item.first == out.start_ms && !item.second.empty()) {
                out.text += " / " + item.second;
                break;
              }
            }
            lyrics.lines.push_back(std::move(out));
          }
          if (!lyrics.plain_text.empty()) {
            lyrics.plain_text += '\n';
          }
          lyrics.plain_text += line.second;
        }

        // YRC 优先：有可解析行就是逐字同步。
        auto word_lines = parse_yrc(yrc_text);
        if (!word_lines.empty()) {
          lyrics.word_synced = true;
          std::string plain;
          for (const auto &line : word_lines) {
            if (!plain.empty()) {
              plain += '\n';
            }
            plain += line.text;
          }
          if (!plain.empty()) {
            lyrics.plain_text = std::move(plain);
          }
          lyrics.lines = std::move(word_lines);
        }

        // pureMusic：平台用「纯音乐，请欣赏」占位，那不是歌词。
        // 清空后由媒体库层翻译成 NotFound（与本地库口径一致）。
        if (field_bool(json, "pureMusic", false) && !lyrics.word_synced) {
          lyrics.lines.clear();
          lyrics.plain_text.clear();
          lyrics.line_synced = false;
        }
        return lyrics;
      });
}

} // namespace api_enhanced
