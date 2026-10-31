#ifndef API_API_ENHANCED_TRANSPORT_H
#define API_API_ENHANCED_TRANSPORT_H

#include "media_library/error.h"
#include "media_library/library.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace api_enhanced {

// api-enhanced 是一个本地 HTTP 服务，本身不持有会话：cookie 随每次请求透传。
// 因此这一层只负责「拼 query + 发 GET + 把 HTTP 故障翻译成 ErrorCode」。
struct NeteaseTransportOptions {
  std::string api_base{"http://127.0.0.1:3000"};
  // 明文凭据：只在构造 query 时使用，绝不写日志、错误或响应。
  std::string cookie;
  int timeout_ms{8000};
  // 可选：透传给 api-enhanced 的 realIP，规避地域风控。
  std::string real_ip;
};

class NeteaseTransport {
public:
  virtual ~NeteaseTransport() = default;

  // path 形如 "/cloudsearch"（不含 query）。query 由本层负责 urlencode 并追加
  // timestamp（绕开 api-enhanced 的 2 分钟同 URL 缓存）。
  // 返回 api-enhanced 的原始 JSON 文本。
  virtual media_library::Result<std::string> get(
      const std::string &path,
      const std::vector<std::pair<std::string, std::string>> &query,
      const media_library::CallContext &ctx) = 0;
};

// 真实实现：复用 webdav 的 HTTP 客户端（本地回环，无凭据，不走凭据查找）。
// api_base 非法时返回 nullptr。
std::shared_ptr<NeteaseTransport>
make_http_transport(const NeteaseTransportOptions &options);

// 测试用：按 path 返回固定 JSON，可注入失败，便于在没有 api-enhanced 的环境里
// 覆盖客户端与媒体库对象的逻辑。
class FakeNeteaseTransport : public NeteaseTransport {
public:
  struct Reply {
    bool ok{true};
    std::string body;
    media_library::ErrorCode code{media_library::ErrorCode::ProviderUnavailable};
  };

  void set_reply(const std::string &path, Reply reply) {
    replies_[path] = std::move(reply);
  }

  // 记录最近一次请求的 path，方便断言。
  const std::string &last_path() const noexcept { return last_path_; }

  media_library::Result<std::string> get(
      const std::string &path,
      const std::vector<std::pair<std::string, std::string>> &query,
      const media_library::CallContext &ctx) override {
    (void)query;
    (void)ctx;
    last_path_ = path;
    const auto it = replies_.find(path);
    if (it == replies_.end()) {
      media_library::Error error = media_library::Error::make(
          media_library::ErrorCode::NotFound, "fake transport has no reply");
      return media_library::Result<std::string>::failure(std::move(error));
    }
    if (!it->second.ok) {
      media_library::Error error =
          media_library::Error::make(it->second.code, "fake transport failure");
      return media_library::Result<std::string>::failure(std::move(error));
    }
    return media_library::Result<std::string>::success(it->second.body);
  }

private:
  std::map<std::string, Reply> replies_;
  std::string last_path_;
};

} // namespace api_enhanced

#endif // API_API_ENHANCED_TRANSPORT_H
