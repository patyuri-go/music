#include "storage.h"

#include <filesystem>
#include <system_error>

#include "api/webdav/webdav.h"
#include "config.h"

namespace storage {

namespace {

namespace fs = std::filesystem;

std::string rstripSlash(std::string s) {
  while (s.size() > 1 && s.back() == '/') {
    s.pop_back();
  }
  return s;
}

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

std::string percentDecode(const std::string &s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hexValue(s[i + 1]);
      const int lo = hexValue(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

std::string encodePath(const std::string &p) {
  std::string out;
  std::string seg;
  for (std::size_t i = 0; i <= p.size(); ++i) {
    if (i == p.size() || p[i] == '/') {
      out += webdav::encodePathSegment(seg);
      if (i < p.size()) {
        out += '/';
      }
      seg.clear();
    } else {
      seg.push_back(p[i]);
    }
  }
  return out;
}

bool isWebdavRoot(const repo::ScanRoot &root) {
  return root.type == "webdav" || webdav::isWebdavUrl(root.path);
}

} // namespace

std::string rootNameOf(const std::string &virtual_path) {
  const auto slash = virtual_path.find('/');
  return slash == std::string::npos ? virtual_path
                                    : virtual_path.substr(0, slash);
}

std::optional<repo::ScanRoot> findRootByName(const std::string &name) {
  repo::ScanRootRepo repo(config::get().db_path.string());
  return repo.byName(name);
}

std::string toReal(const repo::ScanRoot &root,
                   const std::string &virtual_path) {
  const std::string prefix = root.name + "/";
  std::string rel;
  if (virtual_path.rfind(prefix, 0) == 0) {
    rel = virtual_path.substr(prefix.size());
  } else if (virtual_path == root.name) {
    rel.clear();
  } else {
    return virtual_path;
  }

  if (isWebdavRoot(root)) {
    webdav::Url u;
    if (!webdav::parseUrl(root.path, u)) {
      return virtual_path;
    }
    std::string full = percentDecode(u.path);
    if (!rel.empty()) {
      full += "/" + rel;
    }
    const std::string encoded = encodePath(full);
    return (u.https ? "https://" : "http://") + u.host +
           (u.port.empty() ? std::string() : ":" + u.port) + encoded;
  }

  std::string base = rstripSlash(root.path);
  if (rel.empty()) {
    return base;
  }
  return base + "/" + rel;
}

std::string toVirtual(const repo::ScanRoot &root,
                      const std::string &real_path) {
  if (isWebdavRoot(root)) {
    webdav::Url ru;
    webdav::Url bu;
    if (!webdav::parseUrl(real_path, ru) ||
        !webdav::parseUrl(root.path, bu)) {
      return real_path;
    }
    if (ru.host != bu.host || ru.port != bu.port) {
      return real_path;
    }
    const std::string rp = percentDecode(ru.path);
    const std::string bp = rstripSlash(percentDecode(bu.path));
    std::string rel;
    if (rp == bp) {
      rel.clear();
    } else if (rp.rfind(bp + "/", 0) == 0) {
      rel = rp.substr(bp.size() + 1);
    } else {
      return real_path;
    }
    return rel.empty() ? root.name : root.name + "/" + rel;
  }

  std::error_code ec;
  std::string base = rstripSlash(root.path);
  const std::string cb = fs::weakly_canonical(base, ec).string();
  if (ec) {
    return real_path;
  }
  const std::string cr = fs::weakly_canonical(real_path, ec).string();
  if (ec) {
    return real_path;
  }
  std::string rel;
  if (cr == cb) {
    rel.clear();
  } else if (cr.rfind(cb + "/", 0) == 0) {
    rel = cr.substr(cb.size() + 1);
  } else {
    return real_path;
  }
  return rel.empty() ? root.name : root.name + "/" + rel;
}

std::string resolve(const std::string &virtual_path) {
  const std::string name = rootNameOf(virtual_path);
  if (name.empty()) {
    return virtual_path;
  }
  auto root = findRootByName(name);
  if (!root) {
    return virtual_path;
  }
  return toReal(*root, virtual_path);
}

} // namespace storage
