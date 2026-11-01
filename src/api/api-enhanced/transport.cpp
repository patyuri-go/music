#include "api/api-enhanced/transport.h"

#include "api/webdav/webdav.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
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
  // 上游 404 是「这个对象不存在」，不是「后端坏了」。api-enhanced 对缺失的
  // 歌单/专辑直接回 HTTP 404（曲目则回 200 + 空数组），不在这里区分的话
  // 库层的 NotFound 分支永远到不了，主 API 会把「没有」报成 503。
  if (status == 404) {
    return media_library::Error::make(
        media_library::ErrorCode::NotFound,
        "api-enhanced 返回 HTTP 404 (" + path + ")");
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

// 节流装饰器：一个实例只服务一个库。
// 时间基准用 steady_clock——间隔是「等了多久」，不受系统时间调整影响。
std::int64_t monotonic_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

class ThrottledTransport final : public NeteaseTransport {
public:
  ThrottledTransport(std::shared_ptr<NeteaseTransport> inner,
                     int max_concurrency, int min_interval_ms)
      : inner_(std::move(inner)),
        max_concurrency_(max_concurrency > 0 ? max_concurrency : 0),
        min_interval_ms_(min_interval_ms > 0 ? min_interval_ms : 0) {}

  media_library::Result<std::string> get(
      const std::string &path,
      const std::vector<std::pair<std::string, std::string>> &query,
      const media_library::CallContext &ctx) override {
    if (!inner_) {
      return media_library::Result<std::string>::failure(media_library::Error::make(
          media_library::ErrorCode::ProviderUnavailable,
          "netease transport is not configured"));
    }

    // 两道闸门都按「真实请求」计数，所以必须在 inner_->get() 之外持锁。
    // 先等名额再等间隔：占着名额干等间隔会无谓压低并行度。
    if (!acquire_slot(ctx)) {
      return media_library::Result<std::string>::failure(deadline_error());
    }
    media_library::Result<std::string> result = inner_->get(path, query, ctx);
    release_slot();
    return result;
  }

private:
  media_library::Error deadline_error() const {
    media_library::Error error = media_library::Error::make(
        media_library::ErrorCode::Timeout, "netease request deadline exceeded");
    error.retryable = true;
    return error;
  }

  // 拿名额 + 等到允许发起的时刻；成功时时间线已推到下一次。
  bool acquire_slot(const media_library::CallContext &ctx) {
    std::unique_lock<std::mutex> lock(mutex_);
    const std::int64_t deadline =
        ctx.deadline_ms.has_value() ? *ctx.deadline_ms : -1;

    const auto too_late = [this, deadline]() {
      return deadline >= 0 && monotonic_ms() >= deadline;
    };
    const auto wait_until = [this, &lock, deadline](std::int64_t target) {
      if (deadline >= 0 && deadline < target) {
        return false;
      }
      return cv_.wait_until(
          lock,
          std::chrono::steady_clock::time_point(
              std::chrono::milliseconds(target)),
          [this, deadline, target]() {
            // 名额空出来或间隔到点都可能提前醒；deadline 先到就直接放弃。
            if (deadline >= 0 && monotonic_ms() >= deadline) {
              return true;
            }
            const bool slot_free =
                max_concurrency_ == 0 || in_flight_ < max_concurrency_;
            const bool time_reached = monotonic_ms() >= target;
            return slot_free && time_reached;
          });
    };

    while (max_concurrency_ > 0 && in_flight_ >= max_concurrency_) {
      if (too_late()) {
        return false;
      }
      // 名额释放时会 notify_all，这里不需要额外的时间上界：
      // 上面的谓词在 deadline 到点后自己会返回 true。
      cv_.wait(lock, [this, &too_late]() {
        if (too_late()) {
          return true;
        }
        return max_concurrency_ == 0 || in_flight_ < max_concurrency_;
      });
      if (too_late()) {
        return false;
      }
    }

    if (min_interval_ms_ > 0) {
      const std::int64_t target = next_allowed_ms_;
      if (!wait_until(target)) {
        return false;
      }
      if (too_late()) {
        return false;
      }
      // 现在允许发起：把时间线推到「本次发起时刻 + 间隔」。
      next_allowed_ms_ = monotonic_ms() + min_interval_ms_;
    }

    ++in_flight_;
    return true;
  }

  void release_slot() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (in_flight_ > 0) {
      --in_flight_;
    }
    // 名额可能刚空出来；间隔也可能已到点。两种情况都值得唤醒。
    cv_.notify_all();
  }

  std::shared_ptr<NeteaseTransport> inner_;
  int max_concurrency_ = 0;
  int min_interval_ms_ = 0;

  std::mutex mutex_;
  std::condition_variable cv_;
  int in_flight_ = 0;
  std::int64_t next_allowed_ms_ = 0;
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

std::shared_ptr<NeteaseTransport>
make_throttled_transport(std::shared_ptr<NeteaseTransport> inner,
                         int max_concurrency, int min_interval_ms) {
  if (inner == nullptr) {
    return nullptr;
  }
  // 两道闸门都不限时直接返回原对象：少一层虚调用，也便于排查。
  if (max_concurrency <= 0 && min_interval_ms <= 0) {
    return inner;
  }
  return std::make_shared<ThrottledTransport>(std::move(inner),
                                              max_concurrency,
                                              min_interval_ms);
}

} // namespace api_enhanced
