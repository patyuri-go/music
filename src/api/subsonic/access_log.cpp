#include "api/subsonic/access_log.h"

#include <algorithm>
#include <cctype>
#include <iostream>

namespace subsonic {
namespace {

// 取查询参数。Subsonic 客户端一律用小写键，少数实现会混用大小写，
// 这里做一次大小写不敏感匹配。
std::string param(const crow::request &req, const char *key) {
  const std::string want(key);
  for (const auto &k : req.url_params.keys()) {
    if (k.size() != want.size()) {
      continue;
    }
    const bool same =
        std::equal(k.begin(), k.end(), want.begin(), [](char a, char b) {
          return std::tolower(static_cast<unsigned char>(a)) ==
                 std::tolower(static_cast<unsigned char>(b));
        });
    if (same) {
      const char *v = req.url_params.get(k);
      return v ? std::string(v) : std::string();
    }
  }
  return std::string();
}

} // namespace

AccessLog::AccessLog(const crow::request &req, std::string method, bool enabled)
    : enabled_(enabled), method_(std::move(method)),
      started_(std::chrono::steady_clock::now()) {
  if (!enabled_) {
    return;
  }
  client_ = param(req, "c");
  version_ = param(req, "v");
  format_ = param(req, "f");
  if (format_.empty()) {
    format_ = "xml"; // Subsonic 的缺省格式，客户端往往不传 f
  }
}

AccessLog::~AccessLog() { flush(); }

void AccessLog::set_id(const std::string &id) {
  if (!enabled_ || id.empty()) {
    return;
  }
  id_ = id.size() > 64 ? id.substr(0, 64) : id;
}

void AccessLog::set_paging(const std::string &text) {
  if (!enabled_) {
    return;
  }
  paging_ = text;
}

void AccessLog::done(int code, const std::string &error_name) {
  if (finished_) {
    return;
  }
  finished_ = true;
  code_ = code;
  failed_ = code != 0;
  error_name_ = error_name;
}

void AccessLog::flush() {
  if (!enabled_ || flushed_) {
    return;
  }
  flushed_ = true;

  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started_)
                           .count();

  // 没调 done() 就退出了：按失败记 code=0，便于发现忘记标结果的 handler。
  const bool ok = finished_ && !failed_;
  std::cerr << "[subsonic] " << method_ << " method=" << method_
            << " status=" << (ok ? "ok" : "failed") << " code=" << code_
            << " ms=" << ms << " client=" << (client_.empty() ? "-" : client_)
            << " v=" << (version_.empty() ? "-" : version_)
            << " f=" << format_;
  if (!id_.empty()) {
    std::cerr << " id=" << id_;
  }
  if (!paging_.empty()) {
    std::cerr << " " << paging_;
  }
  if (!ok && !error_name_.empty()) {
    std::cerr << " err=" << error_name_;
  }
  std::cerr << "\n";
}

} // namespace subsonic
