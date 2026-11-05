#include "config.h"

#include "crow/json.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace config {
namespace {

Config g_config;
bool g_loaded = false;

// 网易云实体引用由 api-enhanced 的常量生成，库 id 不能改成别的值。
constexpr const char *kNeteaseLibraryId = "netease-main";

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

// 逗号分隔的 library_id 列表（环境变量用）。空项被丢弃。
std::vector<std::string> toIdVector(const std::string &text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    std::string part = (comma == std::string::npos)
                           ? text.substr(start)
                           : text.substr(start, comma - start);
    const std::size_t first = part.find_first_not_of(" \t\r\n");
    const std::size_t last = part.find_last_not_of(" \t\r\n");
    if (first != std::string::npos) {
      part = part.substr(first, last - first + 1);
    } else {
      part.clear();
    }
    if (!part.empty()) {
      out.push_back(std::move(part));
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

bool known_aspect(const std::string &aspect) {
  return aspect == "metadata" || aspect == "lyrics" || aspect == "stream" ||
         aspect == "artwork" || aspect == "search";
}

void check_library_id(const std::string &id, const std::string &what) {
  if (id.empty()) {
    throw std::runtime_error(what + " 的 id 不能为空");
  }
  if (id.find(':') != std::string::npos || id.find('|') != std::string::npos ||
      id.find('=') != std::string::npos) {
    throw std::runtime_error(what + " 的 id 不能包含 ':' '|' '=': " + id);
  }
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

  // 网易云：环境变量是最低优先级的一层，先落默认值，随后被 config.json 覆盖，
  // 最后被命令行覆盖（见 setNetease*）。这样优先级天然是 CLI > 文件 > 环境变量。
  if (const std::string value = envValue("NETEASE_ENABLED"); !value.empty()) {
    loaded.netease.enabled = toBool(value, "NETEASE_ENABLED");
  }
  if (const std::string value = envValue("NETEASE_API_BASE"); !value.empty()) {
    loaded.netease.api_base = value;
  }
  if (const std::string value = envValue("NETEASE_COOKIE"); !value.empty()) {
    loaded.netease.cookie = value;
  }
  if (const std::string value = envValue("NETEASE_DEFAULT_LEVEL");
      !value.empty()) {
    loaded.netease.default_level = value;
  }
  if (const std::string value = envValue("NETEASE_UID"); !value.empty()) {
    loaded.netease.uid = value;
  }
  if (const std::string value = envValue("NETEASE_TIMEOUT_MS"); !value.empty()) {
    loaded.netease.timeout_ms = toInt(value, "NETEASE_TIMEOUT_MS");
  }
  if (const std::string value = envValue("NETEASE_METADATA_TTL_MS");
      !value.empty()) {
    loaded.netease.metadata_ttl_ms = toInt(value, "NETEASE_METADATA_TTL_MS");
  }
  if (const std::string value = envValue("NETEASE_MAX_CONCURRENCY");
      !value.empty()) {
    loaded.netease.max_concurrency = toInt(value, "NETEASE_MAX_CONCURRENCY");
  }
  if (const std::string value = envValue("NETEASE_MIN_REQUEST_INTERVAL_MS");
      !value.empty()) {
    loaded.netease.min_request_interval_ms =
        toInt(value, "NETEASE_MIN_REQUEST_INTERVAL_MS");
  }
  if (const std::string value = envValue("NETEASE_REAL_IP"); !value.empty()) {
    loaded.netease.real_ip = value;
  }

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
    if (json.has("main_port")) {
      loaded.port = static_cast<int>(json["main_port"].i());
    }
    if (json.has("page_size")) {
      loaded.page_size = static_cast<int>(json["page_size"].i());
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
    if (json.has("subsonic")) {
      const auto &sc = json["subsonic"];
      if (sc.has("enabled")) {
        loaded.subsonic.enabled = sc["enabled"].b();
      }
      if (sc.has("port")) {
        loaded.subsonic.port = static_cast<int>(sc["port"].i());
      }
      if (sc.has("server_name")) {
        loaded.subsonic.server_name = sc["server_name"].s();
      }
      if (sc.has("default_page_size")) {
        loaded.subsonic.default_page_size =
            static_cast<int>(sc["default_page_size"].i());
      }
      if (sc.has("log_requests")) {
        loaded.subsonic.log_requests = sc["log_requests"].b();
      }
      if (sc.has("cors_allow_origin")) {
        loaded.subsonic.cors_allow_origin = sc["cors_allow_origin"].s();
      }
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

    // 媒体库：本地库只允许一个；网易云库最多一个；出现未知 type 直接报错。
    if (json.has("libraries")) {
      const auto &libs = json["libraries"];
      if (libs.t() != crow::json::type::List) {
        throw std::runtime_error("libraries 必须是数组");
      }
      int local_count = 0;
      int netease_count = 0;
      for (const auto &item : libs) {
        if (item.t() != crow::json::type::Object) {
          throw std::runtime_error("libraries 的每一项必须是对象");
        }
        const std::string type =
            item.has("type") ? std::string(item["type"].s()) : std::string{};
        const std::string id =
            item.has("id") ? std::string(item["id"].s()) : std::string{};
        if (type == "local") {
          if (++local_count > 1) {
            throw std::runtime_error("libraries 只允许一个 type=local 的本地库");
          }
          if (!id.empty()) {
            loaded.local.id = id;
          }
          if (item.has("follow_symlinks")) {
            loaded.local.follow_symlinks = item["follow_symlinks"].b();
          }
          if (item.has("scan_on_start")) {
            loaded.local.scan_on_start = item["scan_on_start"].b();
          }
          if (item.has("scan_threads")) {
            loaded.local.scan_threads =
                static_cast<int>(item["scan_threads"].i());
          }
          if (item.has("playlist_dir")) {
            loaded.local.playlist_dir = item["playlist_dir"].s();
          }
        } else if (type == "netease") {
          if (++netease_count > 1) {
            throw std::runtime_error("libraries 最多允许一个 type=netease 的库");
          }
          // 出现在 libraries 里即视为启用，除非显式 enabled=false。
          loaded.netease.enabled = true;
          if (!id.empty()) {
            loaded.netease.id = id;
          }
          if (item.has("enabled")) {
            loaded.netease.enabled = item["enabled"].b();
          }
          if (item.has("api_base")) {
            loaded.netease.api_base = item["api_base"].s();
          }
          if (item.has("cookie")) {
            loaded.netease.cookie = item["cookie"].s();
          }
          if (item.has("default_level")) {
            loaded.netease.default_level = item["default_level"].s();
          }
          if (item.has("uid")) {
            const auto &uid = item["uid"];
            // uid 在 JSON 里可能是数字或字符串，两种都接受。
            if (uid.t() == crow::json::type::Number) {
              loaded.netease.uid = std::to_string(uid.i());
            } else if (uid.t() == crow::json::type::String) {
              loaded.netease.uid = uid.s();
            }
          }
          if (item.has("timeout_ms")) {
            loaded.netease.timeout_ms =
                static_cast<int>(item["timeout_ms"].i());
          }
          if (item.has("metadata_ttl_ms")) {
            loaded.netease.metadata_ttl_ms =
                static_cast<int>(item["metadata_ttl_ms"].i());
          }
          if (item.has("max_concurrency")) {
            loaded.netease.max_concurrency =
                static_cast<int>(item["max_concurrency"].i());
          }
          if (item.has("min_request_interval_ms")) {
            loaded.netease.min_request_interval_ms =
                static_cast<int>(item["min_request_interval_ms"].i());
          }
          if (item.has("real_ip")) {
            loaded.netease.real_ip = item["real_ip"].s();
          }
        } else {
          throw std::runtime_error("libraries 存在未知 type: " + type +
                                   "（只支持 local / netease）");
        }
      }
      if (local_count == 0) {
        throw std::runtime_error("libraries 必须包含一个 type=local 的本地库");
      }
    }

    // 方面优先级：[{ "aspect": "metadata", "priority": ["local-main", ...] }]
    if (json.has("library_priority")) {
      const auto &lp = json["library_priority"];
      if (lp.t() != crow::json::type::List) {
        throw std::runtime_error("library_priority 必须是数组");
      }
      loaded.library_priority.clear();
      for (const auto &item : lp) {
        if (item.t() != crow::json::type::Object) {
          throw std::runtime_error(
              "library_priority 的每一项必须是 {aspect, priority} 对象");
        }
        LibraryPriorityConfig entry;
        if (item.has("aspect")) {
          entry.aspect = item["aspect"].s();
        }
        if (entry.aspect.empty()) {
          throw std::runtime_error("library_priority 项缺少 aspect");
        }
        if (item.has("priority")) {
          const auto &ids = item["priority"];
          if (ids.t() != crow::json::type::List) {
            throw std::runtime_error("library_priority.priority 必须是数组: " +
                                     entry.aspect);
          }
          for (const auto &value : ids) {
            if (value.t() == crow::json::type::String &&
                !std::string(value.s()).empty()) {
              entry.library_ids.push_back(std::string(value.s()));
            }
          }
        }
        loaded.library_priority.push_back(std::move(entry));
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
    loaded.local.follow_symlinks = toBool(follow, "MUSIC_FOLLOW_SYMLINKS");
  }
  const std::string scanOnStart = envValue("MUSIC_SCAN_ON_START");
  if (!scanOnStart.empty()) {
    loaded.local.scan_on_start = toBool(scanOnStart, "MUSIC_SCAN_ON_START");
  }
  const std::string scanThreads = envValue("MUSIC_SCAN_THREADS");
  if (!scanThreads.empty()) {
    loaded.local.scan_threads = toInt(scanThreads, "MUSIC_SCAN_THREADS");
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
  const std::string scEnabled = envValue("MUSIC_SUBSONIC_ENABLED");
  if (!scEnabled.empty()) {
    loaded.subsonic.enabled = toBool(scEnabled, "MUSIC_SUBSONIC_ENABLED");
  }
  const std::string scPort = envValue("MUSIC_SUBSONIC_PORT");
  if (!scPort.empty()) {
    loaded.subsonic.port = toInt(scPort, "MUSIC_SUBSONIC_PORT");
  }
  const std::string scLog = envValue("MUSIC_SUBSONIC_LOG_REQUESTS");
  if (!scLog.empty()) {
    loaded.subsonic.log_requests = toBool(scLog, "MUSIC_SUBSONIC_LOG_REQUESTS");
  }
  // 允许显式置空以关闭 CORS，所以不判 empty()。
  const std::string scCors = envValue("MUSIC_SUBSONIC_CORS_ALLOW_ORIGIN");
  if (std::getenv("MUSIC_SUBSONIC_CORS_ALLOW_ORIGIN") != nullptr) {
    loaded.subsonic.cors_allow_origin = scCors;
  }
  const std::string playlistDir = envValue("MUSIC_PLAYLIST_DIR");
  if (!playlistDir.empty()) {
    loaded.local.playlist_dir = playlistDir;
  }

  if (loaded.port <= 0 || loaded.port > 65535) {
    throw std::runtime_error("main_port 超出范围 (1-65535): " +
                             std::to_string(loaded.port));
  }
  if (loaded.jellyfin_enabled &&
      (loaded.jellyfin_port <= 0 || loaded.jellyfin_port > 65535)) {
    throw std::runtime_error("jellyfin.port 超出范围 (1-65535): " +
                             std::to_string(loaded.jellyfin_port));
  }
  if (loaded.subsonic.enabled) {
    if (loaded.subsonic.port <= 0 || loaded.subsonic.port > 65535) {
      throw std::runtime_error("subsonic.port 超出范围 (1-65535): " +
                               std::to_string(loaded.subsonic.port));
    }
    if (loaded.subsonic.port == loaded.port ||
        (loaded.jellyfin_enabled &&
         loaded.subsonic.port == loaded.jellyfin_port)) {
      throw std::runtime_error("subsonic.port 与其它监听端口冲突: " +
                               std::to_string(loaded.subsonic.port));
    }
  }
  if (loaded.subsonic.default_page_size <= 0 ||
      loaded.subsonic.default_page_size > 500) {
    throw std::runtime_error("subsonic.default_page_size 超出范围 (1-500): " +
                             std::to_string(loaded.subsonic.default_page_size));
  }
  if (loaded.page_size <= 0) {
    throw std::runtime_error("page_size 必须为正数: " +
                             std::to_string(loaded.page_size));
  }
  check_library_id(loaded.local.id, "本地库");
  if (loaded.local.scan_threads < 1 || loaded.local.scan_threads > 256) {
    throw std::runtime_error("本地库 scan_threads 超出范围 (1-256): " +
                             std::to_string(loaded.local.scan_threads));
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
  if (loaded.netease.id != kNeteaseLibraryId) {
    throw std::runtime_error(
        std::string("netease 库 id 必须是 ") + kNeteaseLibraryId +
        "（实体引用由 api-enhanced 生成）: " + loaded.netease.id);
  }
  if (loaded.netease.timeout_ms <= 0) {
    throw std::runtime_error("netease.timeout_ms 必须为正数: " +
                             std::to_string(loaded.netease.timeout_ms));
  }
  if (loaded.netease.metadata_ttl_ms < 0) {
    throw std::runtime_error("netease.metadata_ttl_ms 不能为负数");
  }
  if (loaded.netease.max_concurrency < 1 ||
      loaded.netease.max_concurrency > 64) {
    throw std::runtime_error("netease.max_concurrency 超出范围 (1-64): " +
                             std::to_string(loaded.netease.max_concurrency));
  }
  if (loaded.netease.min_request_interval_ms < 0) {
    throw std::runtime_error("netease.min_request_interval_ms 不能为负数");
  }
  if (loaded.netease.min_request_interval_ms > 60000) {
    throw std::runtime_error(
        "netease.min_request_interval_ms 超出范围 (0-60000): " +
        std::to_string(loaded.netease.min_request_interval_ms));
  }
  if (loaded.netease.enabled && loaded.netease.api_base.empty()) {
    throw std::runtime_error("netease.api_base 不能为空");
  }
  if (loaded.netease.enabled) {
    const std::string &base = loaded.netease.api_base;
    const bool http = base.rfind("http://", 0) == 0;
    const bool https = base.rfind("https://", 0) == 0;
    if (!http && !https) {
      throw std::runtime_error("netease.api_base 必须是 http(s) URL: " + base);
    }
  }

  // 优先级：aspect 必须已知且不重复；同一条里不允许空项与重复项——
  // 重复会让同一库落进两个 tier，排序结果取决于遍历顺序，属于静默错乱。
  {
    std::vector<std::string> seen_aspects;
    for (const LibraryPriorityConfig &entry : loaded.library_priority) {
      if (!known_aspect(entry.aspect)) {
        throw std::runtime_error(
            "library_priority.aspect 未知: " + entry.aspect +
            "（支持 metadata / lyrics / stream / artwork / search）");
      }
      for (const std::string &other : seen_aspects) {
        if (other == entry.aspect) {
          throw std::runtime_error("library_priority.aspect 重复: " +
                                   entry.aspect);
        }
      }
      seen_aspects.push_back(entry.aspect);

      std::vector<std::string> seen;
      seen.reserve(entry.library_ids.size());
      for (const std::string &id : entry.library_ids) {
        if (id.empty()) {
          throw std::runtime_error("library_priority 不能包含空 library_id");
        }
        for (const std::string &other : seen) {
          if (other == id) {
            throw std::runtime_error(
                "library_priority(" + entry.aspect + ") 出现重复的 library_id: " +
                id);
          }
        }
        seen.push_back(id);
      }
    }
  }

  g_config = loaded;
  g_loaded = true;
  return g_config;
}

void setPort(int port) {
  if (port <= 0 || port > 65535) {
    throw std::runtime_error("main_port 超出范围 (1-65535): " +
                             std::to_string(port));
  }
  get();
  g_config.port = port;
}

void setJellyfinPort(int port) {
  if (port <= 0 || port > 65535) {
    throw std::runtime_error("jellyfin.port 超出范围 (1-65535): " +
                             std::to_string(port));
  }
  get();
  g_config.jellyfin_port = port;
}

void setJellyfinEnabled(bool enabled) {
  get();
  g_config.jellyfin_enabled = enabled;
}

void setNeteaseEnabled(bool enabled) {
  get();
  g_config.netease.enabled = enabled;
}

void setNeteaseApi(std::string api_base) {
  if (api_base.empty()) {
    throw std::runtime_error("netease.api_base 不能为空");
  }
  const bool http = api_base.rfind("http://", 0) == 0;
  const bool https = api_base.rfind("https://", 0) == 0;
  if (!http && !https) {
    throw std::runtime_error("netease.api_base 必须是 http(s) URL: " + api_base);
  }
  get();
  g_config.netease.api_base = std::move(api_base);
}

void setNeteaseCookie(std::string cookie) {
  get();
  g_config.netease.cookie = std::move(cookie);
}

void setNeteaseUid(std::string uid) {
  get();
  g_config.netease.uid = std::move(uid);
}

} // namespace config
