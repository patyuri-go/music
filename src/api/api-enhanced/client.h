#ifndef API_API_ENHANCED_CLIENT_H
#define API_API_ENHANCED_CLIENT_H

#include "api/api-enhanced/transport.h"
#include "media_library/error.h"
#include "media_library/library.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace api_enhanced {

struct NeteaseClientOptions {
  // 明文凭据：随每次请求透传，绝不写日志、错误或响应。
  std::string cookie;
  // 可选：透传 realIP。
  std::string real_ip;
  std::string default_level{"exhigh"};
  // 元数据缓存 TTL；song/url 永远不缓存（直链会过期）。
  int metadata_ttl_ms{600000};
  int cache_capacity{512};
};

struct NeteaseLoginStatus {
  bool authenticated{false};
  std::optional<std::int64_t> user_id;
  std::string nickname;
  // VIP 类型：0 普通、11 音乐包、10 畅听包等。仅用于诊断。
  int vip_type{0};
};

// ---------------------------------------------------------------------------
// 轻量 DTO：只保留领域映射需要的字段，避免 media_library 依赖 crow::json。
// 这些结构是「平台响应」的形状，不是领域对象。
// ---------------------------------------------------------------------------

struct NeteaseArtistRef {
  std::int64_t id{0};
  std::string name;
};

struct NeteaseAlbumBrief {
  std::int64_t id{0};
  std::string name;
  std::string pic_url;
  // 合集歌手：优先 artists[]，为空时回退 artist 单对象。
  std::vector<NeteaseArtistRef> artists;
  std::int64_t publish_time_ms{0};
  std::vector<std::string> tags;
};

// 可选音质清单里的一档（l/m/h/sq/hr），不是实际播放音质。
struct NeteaseQuality {
  bool present{false};
  std::int64_t br{0};
  std::int64_t size{0};
  std::int64_t sample_rate{0};
};

struct NeteaseSong {
  std::int64_t id{0};
  std::string name;
  std::int64_t duration_ms{0};
  // 0 免费、1 VIP、4 需购买专辑、8 非会员低音质。
  int fee{0};
  // no == 0 表示缺省。
  int no{0};
  // cd 可能是字符串（"1"），也可能是数字；0 表示缺省。
  int cd{0};
  std::int64_t publish_time_ms{0};
  std::vector<std::string> alias;
  std::vector<NeteaseArtistRef> artists;
  // al{} 只给 id/name/picUrl，没有艺术家。
  NeteaseAlbumBrief album;
  NeteaseQuality l;
  NeteaseQuality m;
  NeteaseQuality h;
  NeteaseQuality sq;
  NeteaseQuality hr;
};

struct NeteaseAlbum {
  NeteaseAlbumBrief brief;
  std::vector<NeteaseSong> songs;
};

struct NeteaseArtist {
  std::int64_t id{0};
  std::string name;
  std::string pic_url;
  std::int64_t album_size{0};
  std::int64_t music_size{0};
};

struct NeteaseSongUrl {
  std::string url;
  // "mp3" | "flac" | 其它平台格式。
  std::string type;
  std::int64_t bitrate{0};
  std::int64_t size{0};
  std::int64_t sample_rate{0};
  std::string level_name;
  // 直链有效期，单位秒。
  std::int64_t expi_seconds{0};
  int fee{0};
  int code{0};
  // freeTrialInfo 非空 ⇒ 这是试听片段。
  bool free_trial{false};
  std::int64_t trial_end_seconds{0};
  // 平台给出的歌曲真实时长（试听片段时也是完整时长）。
  std::int64_t time_ms{0};
};

// 逐字歌词的一个词。start_ms 是相对整首歌的绝对毫秒。
struct NeteaseLyricWord {
  std::int64_t start_ms{0};
  std::optional<std::int64_t> end_ms;
  std::string text;
};

struct NeteaseLyricLine {
  std::int64_t start_ms{0};
  std::optional<std::int64_t> end_ms;
  std::string text;
  std::vector<NeteaseLyricWord> words;
};

// /lyric/new 的解析结果。翻译已按时间轴合并进 line.text（"原文 / 译文"）。
struct NeteaseLyrics {
  std::vector<NeteaseLyricLine> lines;
  bool word_synced{false};
  bool line_synced{false};
  std::string plain_text;
};

template <typename T> struct NeteasePage {
  std::vector<T> items;
  bool has_more{false};
};

// api-enhanced 的 JSON 客户端。
// 职责边界：负责拼 query（cookie / realIP / limit 等）、解析 JSON、
// 把「平台限制」翻译成 RestrictionCode；传输故障由 transport 负责。
class NeteaseClient {
public:
  NeteaseClient(std::shared_ptr<NeteaseTransport> transport,
                NeteaseClientOptions options);

  // 健康探测：/login/status 的 account 非空即已登录。
  // 注意该端点的响应体是 {"data":{...}}，account 在 data 下。
  media_library::Result<NeteaseLoginStatus>
  login_status(const media_library::CallContext &ctx);

  // /song/detail?ids=：一次最多 500 个。返回顺序由平台决定。
  media_library::Result<std::vector<NeteaseSong>>
  song_detail(const std::vector<std::int64_t> &ids,
              const media_library::CallContext &ctx);

  // /album?id=：顶层 songs[] 才是曲目（album.songs 可能为空）。
  media_library::Result<NeteaseAlbum>
  album(std::int64_t id, const media_library::CallContext &ctx);

  // /artists?id=
  media_library::Result<NeteaseArtist>
  artist(std::int64_t id, const media_library::CallContext &ctx);

  // /artist/album?id=&limit=&offset= → hotAlbums[] + more
  media_library::Result<NeteasePage<NeteaseAlbumBrief>>
  artist_albums(std::int64_t id, int limit, int offset,
                const media_library::CallContext &ctx);

  // /cloudsearch?type=1
  media_library::Result<NeteasePage<NeteaseSong>>
  search_songs(const std::string &keywords, int limit, int offset,
               const media_library::CallContext &ctx);

  // /cloudsearch?type=10
  media_library::Result<NeteasePage<NeteaseAlbumBrief>>
  search_albums(const std::string &keywords, int limit, int offset,
                const media_library::CallContext &ctx);

  // /cloudsearch?type=100
  media_library::Result<NeteasePage<NeteaseArtist>>
  search_artists(const std::string &keywords, int limit, int offset,
                 const media_library::CallContext &ctx);

  // /song/url/v1?id=&level=：直链短时有效，永不缓存。
  media_library::Result<NeteaseSongUrl>
  song_url(std::int64_t id, const std::string &level,
           const media_library::CallContext &ctx);

  // /lyric/new?id=：lrc / tlyric / yrc 三路，解析成统一的行/词结构。
  // 无歌词时返回空 lines（由媒体库层翻译成 NotFound）。
  media_library::Result<NeteaseLyrics>
  lyric(std::int64_t id, const media_library::CallContext &ctx);

  const NeteaseClientOptions &options() const noexcept { return options_; }

private:
  struct CacheEntry {
    std::string body;
    std::int64_t expires_at_ms{0};
  };

  // 带 TTL 的 GET：path 命中缓存且未过期则不发请求。
  // cacheable=false 用于 song/url 这类一次性直链。
  media_library::Result<std::string>
  get_json(const std::string &path,
           std::vector<std::pair<std::string, std::string>> query,
           const media_library::CallContext &ctx, bool cacheable = true);

  std::shared_ptr<NeteaseTransport> transport_;
  NeteaseClientOptions options_;
  // 同一 client 会被多线程调用（registry 按 max_concurrency 放行）。
  mutable std::mutex mutex_;
  std::unordered_map<std::string, CacheEntry> cache_;
};

} // namespace api_enhanced

#endif // API_API_ENHANCED_CLIENT_H
