#include "api/subsonic/params.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace subsonic {
namespace {

bool same_key(const std::string &a, const char *b) {
  const std::size_t n = std::strlen(b);
  if (a.size() != n) {
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

} // namespace

std::string text(const crow::request &req, const char *name) {
  for (const auto &key : req.url_params.keys()) {
    if (!same_key(key, name)) {
      continue;
    }
    const char *value = req.url_params.get(key);
    return value ? std::string(value) : std::string();
  }
  return std::string();
}

int integer(const crow::request &req, const char *name, int fallback) {
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

Paging paging(const crow::request &req, int default_size) {
  Paging page;
  page.size = integer(req, "size", default_size);
  page.offset = integer(req, "offset", 0);
  page.clamp();
  return page;
}

std::optional<std::string> id_param(const crow::request &req) {
  std::string raw = text(req, "id");
  if (raw.empty()) {
    return std::nullopt;
  }
  return raw;
}

} // namespace subsonic
