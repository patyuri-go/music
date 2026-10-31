#include "api/api-enhanced/transport.h"

#include "api/webdav/webdav.h"

#include <chrono>
#include <cstdint>
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

// query 值一律百分号编码：cookie 里含 '='、';'、'+'，不编码会被后端截断。
std::string build_query(
    const std::vector<std::pair<std::string, std::string>> &query) {
  std::string out;
  for (const auto &item : query) {
    if (!out.empty()) {
      out.push_back('&');
    }
    out.append(webdav::encodePathSegment(item.first));
    out.push_back('=');
    out.append(webdav::encodePathSegment(item.second));
  }
  if (!out.empty()) {
    out.push_back('&');
  }
  // api-enhanced 对同 URL 有 2 分钟缓存，时间戳强制走真实请求。
  out.append("timestamp=");
  out.append(std::to_string(now_ms()));
  return out;
}

// HTTP 状态码 -> 领域错误码。限制是限制、故障是故障：
// 这里只出现故障类，限制类由上层根据 JSON 里的 code/fee 判定。
media_library::Error http_error(int status, const std::string &path) {
  if (status == 0) {
    media_library::Error error = media_library::Error::make(
        media_library::ErrorCode::ProviderUnavailable, "api-enhanced 连接失败");
    error.retryable = true;
    return error;
  }
  if (status == 408 || status == 504) {
    media_library::Error error = media_library::Error::make(
        media_library::ErrorCode::Timeout, "api-enhanced 请求超时");
    error.retryable = true;
    return error;
  }
  if (status == 429) {
    media_library::Error error = media_library::Error::make(
        media_library::ErrorCode::RateLimited, "api-enhanced 限流");
    error.retryable = true;
    return error;
  }
  media_library::Error error = media_library::Error::make(
      media_library::ErrorCode::ProviderUnavailable,
      "api-enhanced 返回 HTTP " + std::to_string(status) + " (" + path + ")");
  error.retryable = status >= 500;
  return error;
}

class HttpTransport final : public NeteaseTransport {
public:
  HttpTransport(webdav::Url base, std::string path_prefix, int timeout_ms)
      : client_(std::move(base)), path_prefix_(std::move(path_prefix)),
        timeout_ms_(timeout_ms) {}

  media_library::Result<std::string> get(
      const std::string &path,
      const std::vector<std::pair<std::string, std::string>> &query,
      const media_library::CallContext &ctx) override {
    (void)ctx;
    // webdav::Client 内部固定 30s 超时，timeout_ms 暂未生效：
    // 真的超时控制要等 N2 换成自己的 asio 客户端。
    (void)timeout_ms_;

    std::string target = path_prefix_;
    target.append(path);
    target.push_back('?');
    target.append(build_query(query));

    const webdav::Response response =
        client_.request("GET", target, {}, std::string());
    if (response.status != 200) {
      return media_library::Result<std::string>::failure(
          http_error(response.status, path));
    }
    return media_library::Result<std::string>::success(response.body);
  }

private:
  webdav::Client client_;
  std::string path_prefix_;
  int timeout_ms_;
};

} // namespace

std::shared_ptr<NeteaseTransport>
make_http_transport(const NeteaseTransportOptions &options) {
  webdav::Url base;
  if (options.api_base.empty() || !webdav::parseUrl(options.api_base, base)) {
    return nullptr;
  }
  // api_base 允许带路径前缀（反代到 /netease 之类）；根路径视为空前缀。
  std::string prefix = base.path;
  if (prefix == "/") {
    prefix.clear();
  }
  while (!prefix.empty() && prefix.back() == '/') {
    prefix.pop_back();
  }
  return std::make_shared<HttpTransport>(std::move(base), std::move(prefix),
                                         options.timeout_ms);
}

} // namespace api_enhanced
