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
