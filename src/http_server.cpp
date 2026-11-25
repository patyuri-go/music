#include "http_server.h"

#include <cstddef>
#include <string>

namespace http_server {
namespace {

int hexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// query 的百分号解码：'+' 视为空格（与 application/x-www-form-urlencoded 一致）。
std::string decodeComponent(const std::string &in, bool plus_is_space) {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (plus_is_space && c == '+') {
      out.push_back(' ');
      continue;
    }
    if (c == '%' && i + 2 < in.size()) {
      const int hi = hexValue(in[i + 1]);
      const int lo = hexValue(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(c);
  }
  return out;
}

} // namespace

ResponsePtr textResponse(int code, std::string body, std::string content_type) {
  auto res = drogon::HttpResponse::newHttpResponse();
  res->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
  res->setContentTypeString(content_type);
  res->setBody(std::move(body));
  return res;
}

Query::Query(const RequestPtr &req) {
  if (!req) {
    return;
  }
  const std::string &raw = req->query();
  std::size_t pos = 0;
  while (pos <= raw.size() && !raw.empty()) {
    const std::size_t amp = raw.find('&', pos);
    const std::size_t end = amp == std::string::npos ? raw.size() : amp;
    if (end > pos) {
      const std::string piece = raw.substr(pos, end - pos);
      const std::size_t eq = piece.find('=');
      QueryParam param;
      if (eq == std::string::npos) {
        param.key = decodeComponent(piece, true);
      } else {
        param.key = decodeComponent(piece.substr(0, eq), true);
        param.value = decodeComponent(piece.substr(eq + 1), true);
      }
      params_.push_back(std::move(param));
    }
    if (amp == std::string::npos) {
      break;
    }
    pos = amp + 1;
  }
}

namespace {

bool sameKey(const std::string &a, const std::string &b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a')
                                             : a[i];
    const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a')
                                             : b[i];
    if (x != y) {
      return false;
    }
  }
  return true;
}

} // namespace

std::optional<std::string> Query::get(const std::string &key) const {
  for (const QueryParam &param : params_) {
    if (sameKey(param.key, key)) {
      return param.value;
    }
  }
  return std::nullopt;
}

std::string Query::getOr(const std::string &key,
                         const std::string &fallback) const {
  const std::optional<std::string> value = get(key);
  return value.has_value() ? *value : fallback;
}

bool Query::has(const std::string &key) const {
  for (const QueryParam &param : params_) {
    if (sameKey(param.key, key)) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> Query::list(const std::string &key) const {
  std::vector<std::string> out;
  for (const QueryParam &param : params_) {
    if (sameKey(param.key, key)) {
      out.push_back(param.value);
    }
  }
  return out;
}

std::string portPrefix(int port) {
  return "/__musicd_" + std::to_string(port);
}

void installPortRouting() {
  drogon::app().registerPreRoutingAdvice([](const RequestPtr &req) {
    if (!req) {
      return;
    }
    const std::string path = req->path();
    // 已经带前缀就不再叠加（防止内部转发被二次改写）。
    if (path.rfind("/__musicd_", 0) == 0) {
      return;
    }
    const int port = static_cast<int>(req->localAddr().toPort());
    // path 是原始（未解码）字符串，setPath 默认会重新编码，必须先关掉。
    req->setPathEncode(false);
    req->setPath(portPrefix(port) + path);
  });
}

void installErrorPages() {
  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const RequestPtr &) {
        auto res = drogon::HttpResponse::newHttpResponse();
        res->setStatusCode(code);
        res->setContentTypeString("application/json; charset=utf-8");
        res->setBody("{\"erro\":true}");
        return res;
      });
}

void listen(int port) { drogon::app().addListener("0.0.0.0", port); }

void setWorkerThreads(std::size_t count) {
  drogon::app().setThreadNum(static_cast<std::size_t>(count));
}

void setQuietLogging() {
  drogon::app().setLogLevel(trantor::Logger::kWarn);
}

} // namespace http_server