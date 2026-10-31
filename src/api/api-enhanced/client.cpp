#include "api/api-enhanced/client.h"

#include "crow/json.h"

#include <chrono>
#include <optional>
#include <utility>

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

} // namespace api_enhanced
