#include "storage.h"

#include <filesystem>
#include <system_error>

#include "config.h"
#include "db.h"

namespace storage {

namespace {

namespace fs = std::filesystem;

std::string rstripSlash(std::string s) {
  while (s.size() > 1 && s.back() == '/') {
    s.pop_back();
  }
  return s;
}

bool isHttpPath(const std::string &path) {
  return path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0;
}

} // namespace

std::string rootNameOf(const std::string &virtual_path) {
  const auto slash = virtual_path.find('/');
  return slash == std::string::npos ? virtual_path
                                    : virtual_path.substr(0, slash);
}

std::optional<repo::ScanRoot> findRootByName(const std::string &name) {
  repo::ScanRootRepo repo(db::mainSession());
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

  std::string base = rstripSlash(root.path);
  if (rel.empty()) {
    return base;
  }
  return base + "/" + rel;
}

std::string toVirtual(const repo::ScanRoot &root,
                      const std::string &real_path) {
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

bool isUnderAnyRoot(const fs::path &target) {
  std::error_code ec;
  const fs::path canonical = fs::weakly_canonical(target, ec);
  if (ec) {
    return false;
  }
  repo::ScanRootRepo roots(db::mainSession());
  for (const auto &root : roots.list()) {
    // WebDAV 扫描根已废弃；残留行不参与本地路径判定。
    if (isHttpPath(root.path)) {
      continue;
    }
    fs::path base = fs::weakly_canonical(root.path, ec);
    if (ec) {
      continue;
    }
    const fs::path rel = canonical.lexically_relative(base);
    if (!rel.empty() && rel.native().rfind("..", 0) != 0) {
      bool escaped = false;
      for (const auto &part : rel) {
        if (part == "..") {
          escaped = true;
          break;
        }
      }
      if (!escaped) {
        return true;
      }
    }
  }
  return false;
}

} // namespace storage