#include "api/subsonic/params.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace subsonic {

std::string text(const http_server::RequestPtr &req, const char *name) {
  const std::optional<std::string> value =
      http_server::Query(req).get(std::string(name));
  return value.has_value() ? *value : std::string();
}

int integer(const http_server::RequestPtr &req, const char *name, int fallback) {
  const std::string raw = text(req, name);
  if (raw.empty()) {
    return fallback;
  }
  try {
    return std::stoi(raw);
  } catch (const std::exception &) {
    return fallback;
  }
}

void Paging::clamp(int max_size, int max_offset) {
  if (size < 0) {
    size = 0;
  }
  size = std::min(size, max_size);
  if (offset < 0) {
    offset = 0;
  }
  offset = std::min(offset, max_offset);
}

std::string Paging::log_text() const {
  return "size=" + std::to_string(size) + " offset=" + std::to_string(offset);
}

Paging paging(const http_server::RequestPtr &req, int default_size) {
  Paging page;
  page.size = integer(req, "size", default_size);
  page.offset = integer(req, "offset", 0);
  page.clamp();
  return page;
}

std::optional<std::string> id_param(const http_server::RequestPtr &req) {
  std::string raw = text(req, "id");
  if (raw.empty()) {
    return std::nullopt;
  }
  return raw;
}

} // namespace subsonic
