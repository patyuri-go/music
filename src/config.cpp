#include "config.h"

#include "crow/json.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace config {
namespace {

Config g_config;
bool g_loaded = false;

std::string envValue(const char *key) {
  const char *value = std::getenv(key);
  return value == nullptr ? std::string{} : std::string{value};
}

int toInt(const std::string &text, const std::string &source) {
  try {
    std::size_t used = 0;
    const long long value = std::stoll(text, &used);
    if (used != text.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return static_cast<int>(value);
  } catch (const std::exception &) {
    throw std::runtime_error(source + " 不是合法整数: " + text);
  }
}

bool toBool(const std::string &text, const std::string &source) {
  if (text == "1" || text == "true" || text == "TRUE") {
    return true;
  }
  if (text == "0" || text == "false" || text == "FALSE") {
    return false;
  }
  throw std::runtime_error(source + " 不是合法布尔值: " + text);
}

std::vector<fs::path> toPathVector(const crow::json::rvalue &arr,
                                   const std::string &source) {
  std::vector<fs::path> out;
  if (!arr || arr.t() != crow::json::type::List) {
    return out;
  }
  for (const auto &item : arr) {
    if (item.t() == crow::json::type::String) {
      out.push_back(std::string(item.s()));
    }
  }
  return out;
}

} // namespace

const Config &get() {
  if (!g_loaded) {
    load();
  }
  return g_config;
}

const Config &load(const std::string &file) {
  Config loaded;

  std::ifstream input(file);
  if (input.is_open()) {
    std::stringstream buffer;
    buffer << input.rdbuf();
    auto json = crow::json::load(buffer.str());
    if (!json) {
      throw std::runtime_error("配置文件不是合法 JSON: " + file);
    }
    if (json.has("db_path")) {
      loaded.db_path = json["db_path"].s();
    }
    if (json.has("static_dir")) {
      loaded.static_dir = json["static_dir"].s();
    }
    if (json.has("port")) {
      loaded.port = static_cast<int>(json["port"].i());
    }
    if (json.has("page_size")) {
      loaded.page_size = static_cast<int>(json["page_size"].i());
    }
    if (json.has("follow_symlinks")) {
      loaded.follow_symlinks = json["follow_symlinks"].b();
    }
    if (json.has("scan_roots")) {
      loaded.scan_roots = toPathVector(json["scan_roots"], "scan_roots");
    }
    if (json.has("scan_on_start")) {
      loaded.scan_on_start = json["scan_on_start"].b();
    }
    if (json.has("scan_threads")) {
      loaded.scan_threads = static_cast<int>(json["scan_threads"].i());
    }
    if (json.has("transcode")) {
      const auto &tc = json["transcode"];
      if (tc.has("enabled")) {
        loaded.transcode_enabled = tc["enabled"].b();
      }
    }
    if (json.has("jellyfin")) {
      const auto &jf = json["jellyfin"];
      if (jf.has("enabled")) {
        loaded.jellyfin_enabled = jf["enabled"].b();
      }
      if (jf.has("port")) {
        loaded.jellyfin_port = static_cast<int>(jf["port"].i());
      }
      if (jf.has("server_name")) {
        loaded.jellyfin_server_name = jf["server_name"].s();
      }
    }
    if (json.has("playlist_dir")) {
      loaded.playlist_dir = json["playlist_dir"].s();
    }
    if (json.has("image_cache")) {
      const auto &cache = json["image_cache"];
      if (cache.has("enabled")) {
        loaded.image_cache.enabled = cache["enabled"].b();
      }
      if (cache.has("dir")) {
        loaded.image_cache.dir = cache["dir"].s();
      }
      if (cache.has("max_bytes")) {
        const long long value = cache["max_bytes"].i();
        if (value >= 0) {
          loaded.image_cache.max_bytes =
              static_cast<std::uintmax_t>(value);
        }
      }
      if (cache.has("max_file_bytes")) {
        const long long value = cache["max_file_bytes"].i();
        if (value >= 0) {
          loaded.image_cache.max_file_bytes =
              static_cast<std::uintmax_t>(value);
        }
      }
      if (cache.has("ttl_days")) {
        loaded.image_cache.ttl_days =
            static_cast<int>(cache["ttl_days"].i());
      }
      if (cache.has("gc_interval_seconds")) {
        loaded.image_cache.gc_interval_seconds =
            static_cast<int>(cache["gc_interval_seconds"].i());
      }
      if (cache.has("revalidate_seconds")) {
        loaded.image_cache.revalidate_seconds =
            static_cast<int>(cache["revalidate_seconds"].i());
      }
    }
    if (json.has("webdav")) {
      const auto &wd = json["webdav"];
      if (wd.has("enabled")) {
        loaded.webdav_enabled = wd["enabled"].b();
      }
      if (wd.has("roots") && wd["roots"].t() == crow::json::type::List) {
        for (const auto &item : wd["roots"]) {
          WebdavRoot root;
          if (item.has("url")) {
            root.url = item["url"].s();
          }
          if (item.has("user")) {
            root.user = item["user"].s();
          }
          if (item.has("pass")) {
            root.pass = item["pass"].s();
          }
          if (!root.url.empty()) {
            loaded.webdav_roots.push_back(root);
          }
        }
      }
    }
  }
  // 文件不存在属正常情况：保留内置默认值，保证零配置可启动。

  const std::string dbPath = envValue("MUSIC_DB");
  if (!dbPath.empty()) {
    loaded.db_path = dbPath;
  }
  const std::string staticDir = envValue("MUSIC_STATIC_DIR");
  if (!staticDir.empty()) {
    loaded.static_dir = staticDir;
  }
  const std::string port = envValue("MUSIC_PORT");
  if (!port.empty()) {
    loaded.port = toInt(port, "MUSIC_PORT");
  }
  const std::string pageSize = envValue("MUSIC_PAGE_SIZE");
  if (!pageSize.empty()) {
    loaded.page_size = toInt(pageSize, "MUSIC_PAGE_SIZE");
  }
  const std::string follow = envValue("MUSIC_FOLLOW_SYMLINKS");
  if (!follow.empty()) {
    loaded.follow_symlinks = toBool(follow, "MUSIC_FOLLOW_SYMLINKS");
  }
  const std::string roots = envValue("MUSIC_ROOTS");
  if (!roots.empty()) {
    loaded.scan_roots.clear();
    std::stringstream ss(roots);
    std::string item;
    while (std::getline(ss, item, ':')) {
      if (!item.empty()) {
        loaded.scan_roots.push_back(item);
      }
    }
  }
  const std::string scanOnStart = envValue("MUSIC_SCAN_ON_START");
  if (!scanOnStart.empty()) {
    loaded.scan_on_start = toBool(scanOnStart, "MUSIC_SCAN_ON_START");
  }
  const std::string scanThreads = envValue("MUSIC_SCAN_THREADS");
  if (!scanThreads.empty()) {
    loaded.scan_threads = toInt(scanThreads, "MUSIC_SCAN_THREADS");
  }
  const std::string transcode = envValue("MUSIC_TRANSCODE");
  if (!transcode.empty()) {
    loaded.transcode_enabled = toBool(transcode, "MUSIC_TRANSCODE");
  }
  const std::string jfEnabled = envValue("MUSIC_JELLYFIN_ENABLED");
  if (!jfEnabled.empty()) {
    loaded.jellyfin_enabled = toBool(jfEnabled, "MUSIC_JELLYFIN_ENABLED");
  }
  const std::string jfPort = envValue("MUSIC_JELLYFIN_PORT");
  if (!jfPort.empty()) {
    loaded.jellyfin_port = toInt(jfPort, "MUSIC_JELLYFIN_PORT");
  }
  const std::string jfName = envValue("MUSIC_JELLYFIN_NAME");
  if (!jfName.empty()) {
    loaded.jellyfin_server_name = jfName;
  }
  const std::string wdEnabled = envValue("MUSIC_WEBDAV_ENABLED");
  if (!wdEnabled.empty()) {
    loaded.webdav_enabled = toBool(wdEnabled, "MUSIC_WEBDAV_ENABLED");
  }
  if (!loaded.webdav_roots.empty()) {
    loaded.webdav_enabled = true;
  }
  const std::string playlistDir = envValue("MUSIC_PLAYLIST_DIR");
  if (!playlistDir.empty()) {
    loaded.playlist_dir = playlistDir;
  }

  if (loaded.port <= 0 || loaded.port > 65535) {
    throw std::runtime_error("port 超出范围 (1-65535): " +
                             std::to_string(loaded.port));
  }
  if (loaded.jellyfin_enabled &&
      (loaded.jellyfin_port <= 0 || loaded.jellyfin_port > 65535)) {
    throw std::runtime_error("jellyfin_port 超出范围 (1-65535): " +
                             std::to_string(loaded.jellyfin_port));
  }
  if (loaded.page_size <= 0) {
    throw std::runtime_error("page_size 必须为正数: " +
                             std::to_string(loaded.page_size));
  }
  if (loaded.scan_threads < 1 || loaded.scan_threads > 256) {
    throw std::runtime_error("scan_threads 超出范围 (1-256): " +
                             std::to_string(loaded.scan_threads));
  }
  if (loaded.image_cache.dir.empty()) {
    throw std::runtime_error("image_cache.dir 不能为空");
  }
  if (loaded.image_cache.ttl_days < 0) {
    throw std::runtime_error("image_cache.ttl_days 不能为负数");
  }
  if (loaded.image_cache.gc_interval_seconds < 1) {
    throw std::runtime_error("image_cache.gc_interval_seconds 必须为正数");
  }
  if (loaded.image_cache.revalidate_seconds < 0) {
    throw std::runtime_error("image_cache.revalidate_seconds 不能为负数");
  }

  g_config = loaded;
  g_loaded = true;
  return g_config;
}

void setPort(int port) {
  if (port <= 0 || port > 65535) {
    throw std::runtime_error("port 超出范围 (1-65535): " + std::to_string(port));
  }
  get();
  g_config.port = port;
}

void setJellyfinPort(int port) {
  if (port <= 0 || port > 65535) {
    throw std::runtime_error("jellyfin_port 超出范围 (1-65535): " +
                             std::to_string(port));
  }
  get();
  g_config.jellyfin_port = port;
}

void setJellyfinEnabled(bool enabled) {
  get();
  g_config.jellyfin_enabled = enabled;
}

} // namespace config
