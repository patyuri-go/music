#include "config.h"
#include <json/json.h>

#include "json_util.h"

#include "database.h"
#include "db.h"
#include "http_server.h"
#include "http_util.h"
#include "image_cache.h"
#include "jellyfin.h"
#include "api/subsonic/subsonic.h"
#include "media_library/error.h"
#include "media_library/library_runtime.h"
#include "tools/lyrics_ttml.h"
#include "media_library/media_runtime.h"
#include "media_library/netease_library.h"
#include "storage_client.h"
#include "stream_http.h"
#include "api/v1/v1.h"
#include "api/api-enhanced/ids.h"
#include "metadata.h"
#include "schema.h"
#include "api/playlist/playlist.h"
#include "service.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

using http_util::jsonResponse;
using http_util::urlDecode;

namespace {

const db::Cell &cellAt(const db::Row &row, std::size_t index) {
  static const db::Cell kNull{db::Null{}};
  return index < row.size() ? row[index] : kNull;
}

std::int64_t intAt(const db::Row &row, std::size_t index) {
  const db::Cell &cell = cellAt(row, index);
  if (const auto *value = std::get_if<std::int64_t>(&cell.value)) {
    return *value;
  }
  if (const auto *value = std::get_if<double>(&cell.value)) {
    return static_cast<std::int64_t>(*value);
  }
  if (const auto *value = std::get_if<std::string>(&cell.value)) {
    try {
      return std::stoll(*value);
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

std::string textAt(const db::Row &row, std::size_t index) {
  const db::Cell &cell = cellAt(row, index);
  if (const auto *value = std::get_if<std::string>(&cell.value)) {
    return *value;
  }
  if (const auto *value = std::get_if<std::int64_t>(&cell.value)) {
    return std::to_string(*value);
  }
  if (const auto *value = std::get_if<double>(&cell.value)) {
    return std::to_string(*value);
  }
  return {};
}

void bootstrap(const std::string &config_file) {
  const auto &cfg = config::load(config_file);

  // 主库后端在启动时一次性确定：本地 SQLite 或远端 PostgreSQL。
  db::init(cfg.database);
  db::Session &session = db::mainSession();
  session.query("SELECT 1");

  const bool use_postgres = session.dialect() == db::Dialect::Postgres;
  if (use_postgres) {
    // PostgreSQL 不做 schema 迁移，只建空库并校验版本。
    schema::ensurePostgresSchema(session);
    std::cout << "[boot] db=postgres://" << cfg.database.postgres.url
              << " static_dir=" << cfg.static_dir
              << " page_size=" << cfg.page_size << std::endl;
  } else {
    schema::migrate(session);
    std::cout << "[boot] db=" << cfg.db_path
              << " static_dir=" << cfg.static_dir
              << " page_size=" << cfg.page_size << std::endl;
  }
  std::cout << "[boot] schema 版本 " << schema::kVersion << "，就绪"
            << std::endl;

  playlist::loadAll();
}

// 静态资源路径限定在 static_dir 内（/web/<path>）。
std::optional<fs::path> resolveWithin(const fs::path &root,
                                      const std::string &relative) {
  std::error_code ec;
  const fs::path base = fs::weakly_canonical(root, ec);
  if (ec) {
    return std::nullopt;
  }
  const fs::path target = fs::weakly_canonical(root / relative, ec);
  if (ec) {
    return std::nullopt;
  }
  const fs::path rel = target.lexically_relative(base);
  if (rel.empty()) {
    return std::nullopt;
  }
  for (const auto &part : rel) {
    if (part == "..") {
      return std::nullopt;
    }
  }
  return target;
}

std::optional<media_library::SourceRef>
api_ref(const std::string &text, media_library::EntityType type) {
  const std::optional<media_library::SourceRef> ref =
      media_library::parse_ref(text);
  if (!ref.has_value() || ref->entity_type != type) {
    return std::nullopt;
  }
  return ref;
}

std::string ref_text(const media_library::SourceRef &ref) {
  return media_library::to_string(ref);
}

// 详情页要完整子项：按游标翻页直到取完（与 Subsonic 门面同一策略）。
template <typename T, typename Fetch> std::vector<T> fetch_all(Fetch fetch) {
  std::vector<T> out;
  std::optional<std::string> cursor;
  for (int guard = 0; guard < 1000; ++guard) {
    media_library::PageRequest page;
    page.limit = 1000000;
    page.cursor = cursor;
    auto result = fetch(page);
    if (!result.ok()) {
      break;
    }
    media_library::Page<T> value = std::move(result).value().value;
    for (T &item : value.items) {
      out.push_back(std::move(item));
    }
    if (!value.next_cursor.has_value()) {
      break;
    }
    cursor = std::move(value.next_cursor);
  }
  return out;
}

Json::Value strings_json(const std::vector<std::string> &values) {
  Json::Value list(Json::arrayValue);
    for (const std::string &value : values) {
    list.append(value);
  }
  return std::move(list);
}

Json::Value
artist_links_json(const std::vector<media_library::ArtistLink> &links) {
  Json::Value list(Json::arrayValue);
  for (const media_library::ArtistLink &link : links) {
    Json::Value item;
    item["id"] = ref_text(link.ref);
    item["name"] = link.name;
    list.append(std::move(item));
  }
  return std::move(list);
}

Json::Value album_artist_links_json(
    const std::vector<media_library::AlbumArtistLink> &links) {
  Json::Value list(Json::arrayValue);
  for (const media_library::AlbumArtistLink &link : links) {
    Json::Value item;
    item["id"] = ref_text(link.ref);
    item["name"] = link.name;
    list.append(std::move(item));
  }
  return std::move(list);
}

std::string artistKeyOf(const std::vector<std::string> &names) {
  std::vector<std::string> sorted;
  for (const auto &n : names) {
    std::string lowered;
    for (const char c : n) {
      lowered.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (!lowered.empty()) {
      sorted.push_back(lowered);
    }
  }
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  std::string key;
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    if (i != 0) {
      key += '|';
    }
    key += sorted[i];
  }
  return key;
}

int ensureArtistId(db::Session &db, const std::string &name) {
  if (name.empty()) {
    return 0;
  }
  db.execute(schema::sql::artistInsert(db.dialect()), {name});
  const auto result = db.query(schema::sql::kArtistByName, {name});
  return result.rows.empty() ? 0 : static_cast<int>(intAt(result.rows.front(), 0));
}

std::vector<int> ensureArtistIds(db::Session &db,
                                 const std::vector<std::string> &names) {
  std::vector<int> ids;
  for (const auto &n : names) {
    const int id = ensureArtistId(db, n);
    if (id != 0) {
      ids.push_back(id);
    }
  }
  return ids;
}

int ensureAlbumId(db::Session &db, const std::string &name,
                  const std::string &key) {
  db.execute(schema::sql::albumUpsert(db.dialect()), {name, key});
  const auto result = db.query(schema::sql::kAlbumByKey, {name, key});
  return result.rows.empty() ? 0 : static_cast<int>(intAt(result.rows.front(), 0));
}

void setTrackArtists(db::Session &db, int track_id,
                     const std::vector<int> &artist_ids) {
  db.execute(schema::sql::kTrackArtistClear,
             {static_cast<std::int64_t>(track_id)});
  std::int64_t position = 0;
  for (const int aid : artist_ids) {
    db.execute(schema::sql::trackArtistInsert(db.dialect()),
               {static_cast<std::int64_t>(track_id),
                static_cast<std::int64_t>(aid), position++});
  }
}

void setAlbumArtists(db::Session &db, int album_id,
                     const std::vector<int> &artist_ids) {
  db.execute(schema::sql::kAlbumArtistClear,
             {static_cast<std::int64_t>(album_id)});
  std::int64_t position = 0;
  for (const int aid : artist_ids) {
    db.execute(schema::sql::albumArtistInsert(db.dialect()),
               {static_cast<std::int64_t>(album_id),
                static_cast<std::int64_t>(aid), position++});
  }
}

Json::Value playlistSummaryJson(const playlist::Playlist &p) {
  Json::Value w;
  w["id"] = p.id;
  w["name"] = p.name;
  w["song_count"] = static_cast<Json::Int64>(playlist::songCount(p));
  w["total_duration_ms"] =
      static_cast<Json::Int64>(playlist::totalDurationMs(p));
  return w;
}

Json::Value playlistDetailJson(const playlist::Playlist &p) {
  Json::Value w = playlistSummaryJson(p);
  Json::Value songs(Json::arrayValue);
  for (const auto &s : p.songs) {
    Json::Value sw;
    sw["path"] = s.path;
    sw["album"] = s.album;
    sw["album_artist"] = s.album_artist;
    sw["matched"] = s.matched;
    sw["track_id"] = s.track_id;
    sw["title"] = s.title;
    sw["artist"] = s.artist;
    songs.append(std::move(sw));
  }
  w["songs"] = songs;
  return w;
}

// ---------------------------------------------------------------------------
// 路由声明 → Drogon 注册
//
// 保留原文件里 `MUSICD_ROUTE(app, path)(lambda)` / `.methods(...)` 的写法，
// 内部把 `<string>`/`<int>`/`<path>` 翻译成 Drogon 占位符：
//   <string>、<int> → {n}（不跨斜杠）
//   <path>          → (.*)（跨斜杠，走正则路由）
// ---------------------------------------------------------------------------

struct RouteSpec {
  std::string pattern;
  bool regex = false;
};

RouteSpec makeRouteSpec(const std::string &raw) {
  RouteSpec spec;
  spec.regex = raw.find("<path>") != std::string::npos;
  int index = 0;
  for (std::size_t i = 0; i < raw.size();) {
    if (raw[i] == '<') {
      const std::size_t close = raw.find('>', i);
      if (close == std::string::npos) {
        spec.pattern.append(raw.substr(i));
        break;
      }
      const std::string name = raw.substr(i + 1, close - i - 1);
      if (spec.regex) {
        spec.pattern += (name == "path") ? "(.*)" : "([^/]*)";
      } else {
        ++index;
        spec.pattern.push_back('{');
        spec.pattern += std::to_string(index);
        spec.pattern.push_back('}');
      }
      i = close + 1;
      continue;
    }
    spec.pattern.push_back(raw[i]);
    ++i;
  }
  return spec;
}

struct RouteBuilder {
  int port;
  RouteSpec spec;
  std::vector<drogon::HttpMethod> verbs{drogon::Get};

  RouteBuilder(int p, std::string raw)
      : port(p), spec(makeRouteSpec(raw)) {}

  template <typename... M> RouteBuilder &methods(M... m) {
    verbs = {m...};
    return *this;
  }

  template <typename F> void operator()(F f) const {
    for (const drogon::HttpMethod method : verbs) {
      if (spec.regex) {
        http_server::onPortRegex(port, method, spec.pattern, f);
      } else {
        http_server::onPort(port, method, spec.pattern, f);
      }
    }
  }

  // 异步 handler：void(req, callback, args...)。用于需要等存储服务的路由。
  template <typename F> void async(F f) const {
    for (const drogon::HttpMethod method : verbs) {
      if (spec.regex) {
        http_server::onPortRegexAsync(port, method, spec.pattern, f);
      } else {
        http_server::onPortAsync(port, method, spec.pattern, f);
      }
    }
  }
};

} // namespace

#define MUSICD_ROUTE(app, url) RouteBuilder((cfg.port), (url))

int main(int argc, char *argv[]) {
  // --config 必须在加载配置之前解析；其余参数都是加载后的覆盖项。
  std::string config_file = "config.json";
  try {
    for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--config") {
        if (i + 1 >= argc) {
          throw std::runtime_error("--config 缺少参数值");
        }
        config_file = argv[++i];
      }
    }
    bootstrap(config_file);
  } catch (const std::exception &e) {
    std::cerr << "[fatal] 启动失败: " << e.what() << std::endl;
    return 1;
  }

  try {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      auto next = [&](const char *name) -> std::string {
        if (i + 1 >= argc) {
          throw std::runtime_error(std::string(name) + " 缺少参数值");
        }
        return argv[++i];
      };
      if (arg == "--config") {
        next("--config");  // 已在 bootstrap 前读取。
      } else if (arg == "--port") {
        config::setPort(std::stoi(next("--port")));
      } else if (arg == "--jellyfin-port") {
        config::setJellyfinPort(std::stoi(next("--jellyfin-port")));
      } else if (arg == "--jellyfin-enabled") {
        const std::string v = next("--jellyfin-enabled");
        config::setJellyfinEnabled(v == "1" || v == "true" || v == "TRUE");
      } else if (arg == "--netease-enabled") {
        const std::string v = next("--netease-enabled");
        config::setNeteaseEnabled(v == "1" || v == "true" || v == "TRUE");
      } else if (arg == "--netease-api") {
        config::setNeteaseApi(next("--netease-api"));
      } else if (arg == "--netease-cookie") {
        // 明文凭据只进内存配置，不回显到任何输出。
        config::setNeteaseCookie(next("--netease-cookie"));
      } else if (arg == "--netease-uid") {
        config::setNeteaseUid(next("--netease-uid"));
      } else if (arg == "--help" || arg == "-h") {
        std::cout << "用法: musicd [选项]\n"
                  << "  --config PATH          配置文件 (默认 config.json)\n"
                  << "  --port N               主服务端口 (默认 5667)\n"
                  << "  --jellyfin-port N      Jellyfin 端口 (默认 9000)\n"
                  << "  --jellyfin-enabled B   是否启用 Jellyfin (1/0)\n"
                  << "  --netease-enabled B    是否启用网易云库 (1/0)\n"
                  << "  --netease-api URL      api-enhanced 地址\n"
                  << "  --netease-cookie C     网易云 cookie（最高优先级）\n"
                  << "  --netease-uid U        网易云用户 ID（歌单读取必需）\n"
                  << "  -h, --help             显示帮助\n";
        return 0;
      } else {
        std::cerr << "[fatal] 未知参数: " << arg << "（用 --help 查看用法）"
                  << std::endl;
        return 1;
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "[fatal] 参数错误: " << e.what() << std::endl;
    return 1;
  }

  const auto &cfg = config::get();

  // 本地库的字节来源：外部存储服务。其余库（网易云等）不走这条链路。
  {
    storage_client::Options storage;
    storage.host = cfg.local.storage_host;
    storage.port = cfg.local.storage_port;
    storage.scheme = cfg.local.storage_scheme;
    storage.token = cfg.local.storage_token;
    storage.timeout_ms = cfg.local.storage_timeout_ms;
    storage.max_concurrent = cfg.local.storage_max_concurrent;
    storage.workers = cfg.local.storage_workers;
    storage_client::initialize(storage);
    std::cout << "[boot] storage=" << storage.scheme << "://" << storage.host
              << ":" << storage.port << " workers=" << storage.workers
              << std::endl;
  }

  image_cache::Options cache_options;
  cache_options.enabled = cfg.image_cache.enabled;
  cache_options.dir = cfg.image_cache.dir;
  cache_options.max_bytes = cfg.image_cache.max_bytes;
  cache_options.max_file_bytes = cfg.image_cache.max_file_bytes;
  cache_options.ttl_days = cfg.image_cache.ttl_days;
  cache_options.gc_interval_seconds = cfg.image_cache.gc_interval_seconds;
  cache_options.revalidate_seconds = cfg.image_cache.revalidate_seconds;
  std::string cache_error;
  if (!image_cache::init(cache_options, cache_error)) {
    std::cerr << "[image_cache] disabled: " << cache_error << std::endl;
  }

  // 主服务与 Jellyfin 兼容层共用同一个 Library 对象。
  try {
    media_library::initialize_library(cfg.local.id, cfg.jellyfin_server_name);
  } catch (const std::exception &e) {
    std::cerr << "[fatal] 媒体库初始化失败: " << e.what() << std::endl;
    return 1;
  }

  // P1 组合根：一次性组装全部库。本地库失败即致命；远端库失败只降级。
  std::vector<media_library::LibrarySetup> setups;
  {
    media_library::MediaRuntimeOptions options;
    options.library_id = cfg.local.id;
    options.display_name = cfg.jellyfin_server_name;
    options.platform = media_library::PlatformKind::Local;
    options.priority = 100;
    setups.push_back(
        media_library::LibrarySetup{media_library::library_ptr(), options});
  }

  if (cfg.netease.enabled) {
    api_enhanced::NeteaseTransportOptions transport;
    transport.api_base = cfg.netease.api_base;
    transport.cookie = cfg.netease.cookie;
    transport.timeout_ms = cfg.netease.timeout_ms;
transport.max_concurrency = cfg.netease.max_concurrency;
transport.min_request_interval_ms = cfg.netease.min_request_interval_ms;
    transport.real_ip = cfg.netease.real_ip;

    api_enhanced::NeteaseClientOptions client;
    client.cookie = cfg.netease.cookie;
    client.real_ip = cfg.netease.real_ip;
    client.default_level = cfg.netease.default_level;
    client.metadata_ttl_ms = cfg.netease.metadata_ttl_ms;

    auto netease = media_library::make_netease_library(
        cfg.netease.id, "网易云音乐", transport, client, cfg.netease.uid);
    if (!netease.ok()) {
      // 配置错误或 api-enhanced 不可用都不该拖垮本地库。
      std::cerr << "[netease] 未启用: "
                << error_code_name(netease.error().code) << " "
                << netease.error().message << std::endl;
    } else {
      media_library::MediaRuntimeOptions options;
      options.library_id = cfg.netease.id;
      options.display_name = "网易云音乐";
      options.platform = media_library::PlatformKind::NetEase;
      // 低于本地库：同实体优先本地文件，远端只做补充。
      options.priority = 50;
      options.limits.max_concurrency = cfg.netease.max_concurrency;
      options.limits.default_timeout_ms = cfg.netease.timeout_ms;
      options.initial_health = netease.value()->health().state;
      options.initial_health_reason = netease.value()->health().reason;
      setups.push_back(
          media_library::LibrarySetup{netease.value(), options});
    }
  }

  std::vector<media_library::PriorityOrder> priority_orders;
  priority_orders.reserve(cfg.library_priority.size());
  for (const auto &entry : cfg.library_priority) {
    priority_orders.push_back(
        media_library::PriorityOrder{entry.aspect, entry.library_ids});
  }

  try {
    media_library::initialize_media_runtime(std::move(setups),
                                            priority_orders);
  } catch (const std::exception &e) {
    std::cerr << "[fatal] 媒体库路由初始化失败: " << e.what() << std::endl;
    return 1;
  }

  MUSICD_ROUTE(app, "/")([&cfg](const http_server::RequestPtr &req) {
    return drogon::HttpResponse::newFileResponse(
        (cfg.static_dir / "index.html").string(), 0, 0, false, "",
        drogon::CT_NONE, "", req);
  });

  MUSICD_ROUTE(app, "/web/<path>")([&cfg](const http_server::RequestPtr &req,
                                        std::string path) {
    auto target = resolveWithin(cfg.static_dir, path);
    if (!target) {
      return http_server::textResponse(403, "forbidden");
    }
    // 目录（典型是 `/web/` 解析到 web 根）不能被 newFileResponse 处理，
    // 它返回 500；原先这里走 404，保持一致。
    std::error_code ec;
    if (!std::filesystem::is_regular_file(*target, ec)) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    return drogon::HttpResponse::newFileResponse(target->string(), 0, 0, false,
                                                 "", drogon::CT_NONE, "", req);
  });

  // ---- 原生统一读门面 /api/v1：直接依赖 MediaRouter + LibraryDirectory ----
  // 这一层只做参数解析与 JSON 序列化，不选源、不组合、不拼字段。
  MUSICD_ROUTE(app, "/api/v1/libraries")([](const http_server::RequestPtr &req) {
    return api_v1::libraries(req);
  });
  MUSICD_ROUTE(app, "/api/v1/providers")([](const http_server::RequestPtr &req) {
    return api_v1::provider_health(req);
  });
  MUSICD_ROUTE(app, "/api/v1/health/providers")([](const http_server::RequestPtr &req) {
    return api_v1::provider_health(req);
  });
  MUSICD_ROUTE(app, "/api/v1/search")([](const http_server::RequestPtr &req) {
    return api_v1::search(req);
  });
  MUSICD_ROUTE(app, "/api/v1/tracks/<string>")([](const http_server::RequestPtr &req,
                                                std::string ref) {
    return api_v1::track(req, ref);
  });
  MUSICD_ROUTE(app, "/api/v1/tracks/<string>/stream")
      .methods(drogon::Get, drogon::Head)
      .async([](const http_server::RequestPtr &req,
                std::function<void(const http_server::ResponsePtr &)> callback,
                std::string ref) {
        api_v1::track_stream(req, ref, std::move(callback));
      });
  MUSICD_ROUTE(app, "/api/v1/tracks/<string>/lyrics")([](const http_server::RequestPtr &req,
                                                       std::string ref) {
    return api_v1::track_lyrics(req, ref);
  });
  MUSICD_ROUTE(app, "/api/v1/tracks/<string>/artwork")
      .methods(drogon::Get, drogon::Head)
      .async([](const http_server::RequestPtr &req,
                std::function<void(const http_server::ResponsePtr &)> callback,
                std::string ref) {
        api_v1::track_artwork(req, ref, std::move(callback));
      });
  MUSICD_ROUTE(app, "/api/v1/albums/<string>")([](const http_server::RequestPtr &req,
                                                std::string ref) {
    return api_v1::album(req, ref);
  });
  MUSICD_ROUTE(app, "/api/v1/albums/<string>/tracks")([](const http_server::RequestPtr &req,
                                                       std::string ref) {
    return api_v1::album_tracks(req, ref);
  });
  MUSICD_ROUTE(app, "/api/v1/artists/<string>")([](const http_server::RequestPtr &req,
                                                 std::string ref) {
    return api_v1::artist(req, ref);
  });
  MUSICD_ROUTE(app, "/api/v1/artists/<string>/albums")([](const http_server::RequestPtr &req,
                                                        std::string ref) {
    return api_v1::artist_albums(req, ref);
  });
  MUSICD_ROUTE(app, "/api/v1/playlists")([](const http_server::RequestPtr &req) {
    return api_v1::playlists(req);
  });
  MUSICD_ROUTE(app, "/api/v1/playlists/<string>")([](const http_server::RequestPtr &req,
                                                   std::string ref) {
    return api_v1::playlist(req, ref);
  });

  // ---- 实体链接：读写都以 SourceRef 为句柄，内部 link_id 不出进程 ----
  MUSICD_ROUTE(app, "/api/v1/entity-links")
      .methods(drogon::Post)([](const http_server::RequestPtr &req) {
        return api_v1::entity_link_create(req);
      });
  MUSICD_ROUTE(app, "/api/v1/entity-links/<string>")([](const http_server::RequestPtr &req,
                                                      std::string ref) {
    return api_v1::entity_link(req, ref);
  });
  MUSICD_ROUTE(app, "/api/v1/entity-links/<string>")
      .methods(drogon::Delete)([](const http_server::RequestPtr &req,
                                   std::string ref) {
        return api_v1::entity_link_delete(req, ref);
      });
  MUSICD_ROUTE(app, "/api/v1/entity-links/<string>/order")
      ([](const http_server::RequestPtr &req, std::string ref) {
        return api_v1::entity_link_order(req, ref);
      });
  MUSICD_ROUTE(app, "/api/v1/entity-links/<string>/members/<string>")
      .methods(drogon::Delete)([](const http_server::RequestPtr &req, std::string anchor,
                                   std::string member) {
        return api_v1::entity_link_remove_member(req, anchor, member);
      });
  MUSICD_ROUTE(app, "/api/v1/entity-links/<string>/overrides")
      .methods(drogon::Put)([](const http_server::RequestPtr &req, std::string ref) {
        return api_v1::entity_link_set_overrides(req, ref);
      });
  MUSICD_ROUTE(app, "/api/v1/entity-links/<string>/overrides/<string>")
      .methods(drogon::Delete)([](const http_server::RequestPtr &req, std::string ref,
                                   std::string aspect) {
        return api_v1::entity_link_clear_override(req, ref, aspect);
      });

  // ---- 播放会话：open / 查询 / 刷新 / 关闭 / 字节流 ----
  MUSICD_ROUTE(app, "/api/v1/playback/<string>")
      .methods(drogon::Post)([](const http_server::RequestPtr &req, std::string ref) {
        return api_v1::playback_open(req, ref);
      });
  MUSICD_ROUTE(app, "/api/v1/playback/<string>")([](const http_server::RequestPtr &req,
                                                 std::string session_id) {
    return api_v1::playback_session(req, session_id);
  });
  MUSICD_ROUTE(app, "/api/v1/playback/<string>")
      .methods(drogon::Delete)([](const http_server::RequestPtr &req,
                                   std::string session_id) {
        return api_v1::playback_close(req, session_id);
      });
  MUSICD_ROUTE(app, "/api/v1/playback/<string>/refresh")
      .methods(drogon::Post)([](const http_server::RequestPtr &req,
                                 std::string session_id) {
        return api_v1::playback_refresh(req, session_id);
      });
  MUSICD_ROUTE(app, "/api/v1/playback/<string>/refresh")([](const http_server::RequestPtr &req,
                                                         std::string session_id) {
    return api_v1::playback_refresh(req, session_id);
  });
  MUSICD_ROUTE(app, "/api/v1/playback/<string>/stream")
      .methods(drogon::Get, drogon::Head)(
          [](const http_server::RequestPtr &req, std::string session_id) {
            return api_v1::playback_stream(req, session_id);
          });

  MUSICD_ROUTE(app, "/api/query")
      .methods(drogon::Post)([](const http_server::RequestPtr &req) {
        const Json::Value json = json_util::parseOrNull(req->body());
        if (json.isNull() || !json.isMember("type")) {
          return jsonResponse(400, "{\"erro\":true}");
        }
        Service service;
        auto result = service.query(json);
        if (!result) {
          return jsonResponse(404, "{}");
        }
        return jsonResponse(200, json_util::dump(*result));
      });

  // 读接口：id 是 SourceRef 规范串；详情一律经 MediaRouter 取领域模型。
  MUSICD_ROUTE(app, "/api/track/<string>")([](std::string id_text) {
    const std::optional<media_library::SourceRef> ref =
        api_ref(id_text, media_library::EntityType::Track);
    if (!ref.has_value()) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    const auto track = media_library::media_router().resolve_track(*ref, {});
    if (!track.ok()) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    const media_library::Track &value = track.value().value;
    Json::Value out;
    out["id"] = ref_text(value.ref);
    out["title"] = value.title;
    out["artists"] = artist_links_json(value.artists);
    out["album_artists"] = album_artist_links_json(value.album_artists);
    out["album_id"] = value.album.has_value() ? ref_text(value.album->ref)
                                              : std::string();
    out["album_name"] = value.album.has_value() ? value.album->title
                                               : std::string();
    out["track_no"] = value.track_no.value_or(0);
    out["disc_no"] = value.disc_no.value_or(0);
    out["year"] = value.year.value_or(0);
    out["genre"] = value.genres.empty() ? std::string() : value.genres.front();
    out["genres"] = strings_json(value.genres);
    out["rating"] = value.user_data.rating;
    out["play_count"] = value.user_data.play_count;
    out["added_at"] = value.user_data.added_at;
    out["last_played"] = value.user_data.last_played;
    out["duration_ms"] = value.duration_ms.value_or(std::int64_t{0});
    out["format"] = value.audio.format;
    out["size_bytes"] = value.audio.size_bytes.value_or(std::int64_t{0});
    out["bitrate"] = value.audio.bitrate_kbps.value_or(0);
    out["sample_rate"] = value.audio.sample_rate.value_or(0);
    out["channels"] = value.audio.channels.value_or(0);
    out["has_lyrics"] = value.lyrics_available;
    out["playable"] = value.playable;
    out["artwork"] = value.artwork.has_value() ? ref_text(value.ref)
                                               : std::string();
    return jsonResponse(200, json_util::dump(out));
  });

  MUSICD_ROUTE(app, "/api/track/<int>").methods(drogon::Delete)([](int id) {
    repo::TrackRepo repo(db::mainSession());
    if (repo.remove(id)) {
      return jsonResponse(200, "{\"status\":\"ok\"}");
    }
    return jsonResponse(404, "{\"erro\":true}");
  });

  MUSICD_ROUTE(app, "/api/album/<string>")([](std::string id_text) {
    const std::optional<media_library::SourceRef> ref =
        api_ref(id_text, media_library::EntityType::Album);
    if (!ref.has_value()) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    const auto album = media_library::media_router().get_album(*ref, {});
    if (!album.ok()) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    const media_library::Album &value = album.value().value;
    const std::vector<media_library::Track> tracks =
        fetch_all<media_library::Track>(
            [ref](const media_library::PageRequest &page) {
              return media_library::media_router().list_album_tracks(*ref, page,
                                                                     {});
            });

    Json::Value out;
    out["id"] = ref_text(value.ref);
    out["name"] = value.title;
    out["album_artists"] = album_artist_links_json(value.artists);
    out["year"] = value.year.value_or(0);
    out["genres"] = strings_json(value.genres);
    out["artwork"] = value.artwork.has_value() ? ref_text(value.ref)
                                               : std::string();
    Json::Value track_list(Json::arrayValue);
      for (const media_library::Track &track : tracks) {
      Json::Value item;
      item["id"] = ref_text(track.ref);
      item["title"] = track.title;
      item["track_no"] = track.track_no.value_or(0);
      item["disc_no"] = track.disc_no.value_or(0);
      item["duration_ms"] = track.duration_ms.value_or(std::int64_t{0});
      track_list.append(std::move(item));
    }
    out["tracks"] = std::move(track_list);
    return jsonResponse(200, json_util::dump(out));
  });

  MUSICD_ROUTE(app, "/api/album/<int>").methods(drogon::Delete)([](int id) {
    try {
      repo::AlbumRepo album_repo(db::mainSession());
      if (!album_repo.byId(id)) {
        return jsonResponse(404, "{\"erro\":true}");
      }
      db::Session &db = db::mainSession();

      int removed = 0;
      {
        const auto count =
            db.query("SELECT COUNT(*) FROM track WHERE album_id = ?",
                     {static_cast<std::int64_t>(id)});
        if (!count.rows.empty()) {
          removed = static_cast<int>(intAt(count.rows.front(), 0));
        }
      }
      db.execute("DELETE FROM track WHERE album_id = ?",
                 {static_cast<std::int64_t>(id)});
      db.execute(schema::sql::kAlbumDelete, {static_cast<std::int64_t>(id)});

      Json::Value out;
      out["status"] = "ok";
      out["tracks_removed"] = removed;
      return jsonResponse(200, json_util::dump(out));
    } catch (const std::exception &e) {
      return jsonResponse(500, "{\"erro\":true}");
    }
  });

  MUSICD_ROUTE(app, "/api/album/<int>/update")
      .methods(drogon::Post)([](const http_server::RequestPtr &req, int id) {
        try {
          repo::AlbumRepo album_repo(db::mainSession());
          auto album = album_repo.byId(id);
          if (!album) {
            return jsonResponse(404, "{\"erro\":true}");
          }

          const Json::Value body = json_util::parseOrNull(req->body());
          if (body.isNull()) {
            return jsonResponse(400, "{\"erro\":true}");
          }

          db::Session &db = db::mainSession();

          const std::string new_name =
              body.isMember("name") ? body["name"].asString() : album->name;

          std::vector<std::string> artist_names;
          if (body.isMember("album_artist")) {
            artist_names = metadata::splitArtists(body["album_artist"].asString());
          } else {
            repo::ArtistRepo artist_repo(db::mainSession());
            for (const int aid : album->artist_ids) {
              if (auto a = artist_repo.byId(aid)) {
                artist_names.push_back(a->name);
              }
            }
          }
          const std::string key = artistKeyOf(artist_names);
          const std::vector<int> artist_ids = ensureArtistIds(db, artist_names);

          int existing = 0;
          {
            const auto query =
                db.query(schema::sql::kAlbumByKey, {new_name, key});
            if (!query.rows.empty()) {
              existing = static_cast<int>(intAt(query.rows.front(), 0));
            }
          }
          const int target = (existing != 0 && existing != id) ? existing : id;

          if (target != id) {
            db.execute(schema::sql::kTrackReassignAlbum,
                       {static_cast<std::int64_t>(target),
                        static_cast<std::int64_t>(id)});

            const auto count =
                db.query("SELECT COUNT(*) FROM track WHERE album_id = ?",
                         {static_cast<std::int64_t>(id)});
            if (!count.rows.empty() && intAt(count.rows.front(), 0) == 0) {
              db.execute(schema::sql::kAlbumDelete,
                         {static_cast<std::int64_t>(id)});
            }
          } else {
            db.execute(schema::sql::kAlbumUpdateNameKey,
                       {new_name, key, static_cast<std::int64_t>(id)});
          }

          setAlbumArtists(db, target, artist_ids);

          if (body.isMember("year")) {
            db.execute(schema::sql::kAlbumUpdateYear,
                       {static_cast<std::int64_t>(body["year"].asInt64()),
                        static_cast<std::int64_t>(target)});
          }
          if (body.isMember("cover_path")) {
            // 仅记录外部写入方给出的路径，本进程不再读取它。
            const std::string cover_value = body["cover_path"].asString();
            db.execute(schema::sql::kAlbumUpdateCover,
                       {cover_value, static_cast<std::int64_t>(target)});
          }

          Json::Value out;
          out["status"] = "ok";
          out["album_id"] = target;
          out["merged"] = (target != id);
          return jsonResponse(200, json_util::dump(out));
        } catch (const std::exception &) {
          return jsonResponse(409, "{\"erro\":true,\"reason\":\"conflict\"}");
        }
      });

  MUSICD_ROUTE(app, "/api/artist/<string>")([](std::string id_text) {
    // 艺术家与专辑艺术家是两种引用，同一个底层艺术家两边都能打开。
    const std::optional<media_library::SourceRef> parsed =
        media_library::parse_ref(id_text);
    if (!parsed.has_value() ||
        (parsed->entity_type != media_library::EntityType::Artist &&
         parsed->entity_type != media_library::EntityType::AlbumArtist)) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    const media_library::SourceRef ref =
        parsed->entity_type == media_library::EntityType::AlbumArtist
            ? media_library::as_artist_ref(*parsed)
            : *parsed;
    const auto artist = media_library::media_router().get_artist(ref, {});
    if (!artist.ok()) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    const media_library::Artist &value = artist.value().value;
    const std::vector<media_library::Album> albums =
        fetch_all<media_library::Album>(
            [ref](const media_library::PageRequest &page) {
              return media_library::media_router().list_artist_albums(ref, page,
                                                                      {});
            });

    Json::Value out;
    out["id"] = ref_text(value.ref);
    out["name"] = value.name;
    out["sort_name"] = value.sort_name;
    out["artwork"] = value.artwork.has_value() ? ref_text(value.ref)
                                               : std::string();
    Json::Value album_list(Json::arrayValue);
      for (const media_library::Album &album : albums) {
      Json::Value item;
      item["id"] = ref_text(album.ref);
      item["name"] = album.title;
      item["year"] = album.year.value_or(0);
      album_list.append(std::move(item));
    }
    out["albums"] = std::move(album_list);
    return jsonResponse(200, json_util::dump(out));
  });


  MUSICD_ROUTE(app, "/api/playlists/refresh").methods(drogon::Post)([] {
    auto r = playlist::refresh();
    Json::Value out;
    out["playlists"] = r.playlists;
    out["songs"] = r.songs;
    out["matched"] = r.matched;
    Json::Value unmatched(Json::arrayValue);
    for (const auto &u : r.unmatched) {
      Json::Value uw;
      uw["playlist"] = u.playlist;
      uw["path"] = u.song.path;
      uw["album"] = u.song.album;
      uw["album_artist"] = u.song.album_artist;
      unmatched.append(std::move(uw));
    }
    out["unmatched"] = unmatched;
    return jsonResponse(200, json_util::dump(out));
  });

  MUSICD_ROUTE(app, "/api/playlists")([] {
    auto playlists = playlist::list();
    Json::Value out(Json::arrayValue);
    int i = 0;
    for (const auto &p : playlists) {
      out[i] = playlistSummaryJson(p);
      ++i;
    }
    return jsonResponse(200, json_util::dump(out));
  });

  MUSICD_ROUTE(app, "/api/playlists")
      .methods(drogon::Post)([](const http_server::RequestPtr &req) {
        const Json::Value json = json_util::parseOrNull(req->body());
        if (json.isNull() || !json.isMember("name")) {
          return jsonResponse(400, "{\"erro\":true}");
        }
        playlist::Playlist p = playlist::create(std::string(json["name"].asString()));
        return jsonResponse(200, json_util::dump(playlistDetailJson(p)));
      });

  MUSICD_ROUTE(app, "/api/playlists/<string>")([](const http_server::RequestPtr &,
                                                std::string id) {
    playlist::Playlist p;
    if (!playlist::get(id, p)) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    return jsonResponse(200, json_util::dump(playlistDetailJson(p)));
  });

  MUSICD_ROUTE(app, "/api/playlists/<string>")
      .methods(drogon::Delete)([](const http_server::RequestPtr &, std::string id) {
        if (!playlist::remove(id)) {
          return jsonResponse(404, "{\"erro\":true}");
        }
        return jsonResponse(200, "{\"status\":\"ok\"}");
      });

  MUSICD_ROUTE(app, "/api/playlists/<string>/songs")
      .methods(drogon::Post)([](const http_server::RequestPtr &req, std::string id) {
        const Json::Value json = json_util::parseOrNull(req->body());
        if (json.isNull()) {
          return jsonResponse(400, "{\"erro\":true}");
        }
        std::string err;
        bool ok = false;
        if (json.isMember("trackId")) {
          ok = playlist::addTrack(id, static_cast<int>(json["trackId"].asInt64()), err);
        } else if (json.isMember("path")) {
          ok = playlist::addPath(id, std::string(json["path"].asString()), err);
        } else {
          err = "missing path or trackId";
        }
        if (!ok) {
          return jsonResponse(400, "{\"erro\":true,\"reason\":\"" + err + "\"}");
        }
        playlist::Playlist p;
        playlist::get(id, p);
        return jsonResponse(200, json_util::dump(playlistDetailJson(p)));
      });

  MUSICD_ROUTE(app, "/api/playlists/<string>/songs/<int>")
      .methods(drogon::Delete)([](const http_server::RequestPtr &, std::string id,
                                   int index) {
        if (!playlist::removeAt(id, index)) {
          return jsonResponse(404, "{\"erro\":true}");
        }
        return jsonResponse(200, "{\"status\":\"ok\"}");
      });

  // TODO(legacy): 跨库聚合后由媒体库层直接提供最近播放列表。
  MUSICD_ROUTE(app, "/api/recent")([] {
    media_library::CatalogSnapshot snap;
    media_library::compat_snapshot(snap);
    Json::Value out(Json::arrayValue);
    int i = 0;
    for (const int internal_id : snap.recent_play_track_ids) {
      auto item = Service().trackById(internal_id);
      if (!item) {
        continue;
      }
      out[i] = std::move(*item);
      ++i;
      if (config::get().page_size > 0 &&
          i >= config::get().page_size) {
        break;
      }
    }
    return jsonResponse(200, json_util::dump(out));
  });

  MUSICD_ROUTE(app, "/api/album/<string>/cover")
      .methods(drogon::Get, drogon::Head)
      .async([](const http_server::RequestPtr &req,
                std::function<void(const http_server::ResponsePtr &)> callback,
                std::string id_text) {
        const std::optional<media_library::SourceRef> ref =
            api_ref(id_text, media_library::EntityType::Album);
        if (!ref.has_value()) {
          callback(jsonResponse(404, "{\"erro\":true}"));
          return;
        }
        media_library::ArtworkRef artwork;
        artwork.owner = *ref;
        stream_http::serve_artwork_async(
            req, artwork, req->isHead(), std::move(callback));
      });

  MUSICD_ROUTE(app, "/api/track/<int>/file")
      .methods(drogon::Get, drogon::Head)
      .async([](const http_server::RequestPtr &req,
                std::function<void(const http_server::ResponsePtr &)> callback,
                int id) {
        // TODO(legacy): 对外 id 仍是库内 int；改走 SourceRef 后这条路由可删除。
        const auto ref = media_library::compat_ref_for(
            media_library::EntityType::Track, id);
        if (!ref) {
          callback(jsonResponse(404, "{\"erro\":true}"));
          return;
        }
        stream_http::serve_track_async(
            req, *ref, req->isHead(), std::move(callback));
      });

  MUSICD_ROUTE(app, "/api/track/<int>/play")
      .methods(drogon::Post)([](int id) {
        repo::TrackRepo repo(db::mainSession());
        if (repo.recordPlay(id)) {
          return jsonResponse(200, "{\"status\":\"ok\"}");
        }
        return jsonResponse(404, "{\"erro\":true}");
      });

  MUSICD_ROUTE(app, "/api/track/<string>/lyrics")([](std::string id_text) {
    const std::optional<media_library::SourceRef> ref =
        api_ref(id_text, media_library::EntityType::Track);
    if (!ref.has_value()) {
      return jsonResponse(404, "{\"erro\":true}");
    }
    Json::Value out;
    out["text"] = "";
    out["synced"] = false;
    const auto lyrics = media_library::media_router().resolve_lyrics(*ref, {});
    if (lyrics.ok()) {
      const auto &value = lyrics.value().value;
      media_library::LyricsTtmlMeta meta;
      const auto track =
          media_library::media_router().resolve_track(*ref, {});
      if (track.ok()) {
        const media_library::Track &info = track.value().value;
        meta.music_name = info.title;
        if (info.album.has_value()) {
          meta.album = info.album->title;
        }
        for (const media_library::ArtistLink &artist : info.artists) {
          meta.artists.push_back(artist.name);
        }
        meta.duration_ms = info.duration_ms;
      }
      out["text"] = media_library::lyrics_to_ttml(value, meta);
      out["synced"] = media_library::is_synced(value);
    }
    return jsonResponse(200, json_util::dump(out));
  });

  MUSICD_ROUTE(app, "/api/track/<int>/lyrics")
      .methods(drogon::Post)([](const http_server::RequestPtr &req, int id) {
        repo::TrackRepo repo(db::mainSession());
        if (!repo.byId(id)) {
          return jsonResponse(404, "{\"erro\":true}");
        }
        const Json::Value body = json_util::parseOrNull(req->body());
        if (body.isNull() || !body.isMember("text")) {
          return jsonResponse(400, "{\"erro\":true}");
        }
        const std::string text = body["text"].asString();
        db::Session &db = db::mainSession();
        if (text.empty()) {
          db.execute("DELETE FROM track_lyrics WHERE track_id = ?",
                     {static_cast<std::int64_t>(id)});
        } else {
          db.execute(schema::sql::trackLyricsUpsert(db.dialect()),
                     {static_cast<std::int64_t>(id), text,
                      static_cast<std::int64_t>(
                          metadata::isSyncedLyrics(text) ? 1 : 0)});
        }
        return jsonResponse(200, "{\"status\":\"ok\"}");
      });

  MUSICD_ROUTE(app, "/api/track/<int>/update")
      .methods(drogon::Post)([](const http_server::RequestPtr &req, int id) {
        const Json::Value json = json_util::parseOrNull(req->body());
        if (json.isNull()) {
          return jsonResponse(400, "{\"erro\":true}");
        }

        repo::TrackRepo repo(db::mainSession());
        auto track = repo.byId(id);
        if (!track) {
          return jsonResponse(404, "{\"erro\":true}");
        }

        repo::Track t = *track;
        if (json.isMember("title")) {
          t.title = json["title"].asString();
        }
        db::Session &db = db::mainSession();
        repo::ArtistRepo artist_repo(db::mainSession());
        repo::AlbumRepo album_repo(db::mainSession());

        std::vector<std::string> artist_names;
        if (json.isMember("artist")) {
          artist_names = metadata::splitArtists(json["artist"].asString());
        }

        std::vector<std::string> album_artist_names;
        if (json.isMember("album_artist")) {
          album_artist_names = metadata::splitArtists(json["album_artist"].asString());
        } else if (!artist_names.empty()) {
          album_artist_names = artist_names;
        }

        std::string album_name;
        if (json.isMember("album")) {
          album_name = json["album"].asString();
        }

        if (!artist_names.empty()) {
          setTrackArtists(db, id, ensureArtistIds(db, artist_names));
        }

        if (json.isMember("album") || json.isMember("album_artist")) {
          if (album_name.empty() && t.album_id > 0) {
            if (auto a = album_repo.byId(t.album_id)) {
              album_name = a->name;
            }
          }
          if (album_artist_names.empty() && t.album_id > 0) {
            for (const int aid : album_repo.artistIdsByAlbumId(t.album_id)) {
              if (auto a = artist_repo.byId(aid)) {
                album_artist_names.push_back(a->name);
              }
            }
          }
          const int album_id =
              ensureAlbumId(db, album_name, artistKeyOf(album_artist_names));
          if (album_id != 0) {
            t.album_id = album_id;
            setAlbumArtists(db, album_id,
                            ensureArtistIds(db, album_artist_names));
          }
        }
        if (json.isMember("year") && t.album_id > 0) {
          db.execute(schema::sql::kAlbumUpdateYear,
                     {static_cast<std::int64_t>(json["year"].asInt64()),
                      static_cast<std::int64_t>(t.album_id)});
        }
        if (json.isMember("genre")) {
          std::string genre_name = json["genre"].asString();
          db.execute("DELETE FROM track_genre WHERE track_id = ?",
                     {static_cast<std::int64_t>(id)});
          if (!genre_name.empty()) {
            db.execute(schema::sql::genreInsert(db.dialect()), {genre_name});
            const auto query =
                db.query(schema::sql::kGenreByName, {genre_name});
            if (!query.rows.empty()) {
              db.execute(schema::sql::trackGenreInsert(db.dialect()),
                         {static_cast<std::int64_t>(id),
                          intAt(query.rows.front(), 0)});
            }
          }
        }
        if (json.isMember("disc_no")) {
          t.disc_no = json["disc_no"].asInt64();
        }
        if (json.isMember("track_no")) {
          t.track_no = json["track_no"].asInt64();
        }
        if (json.isMember("duration_ms")) {
          t.duration_ms = json["duration_ms"].asInt64();
        }
        if (json.isMember("bitrate")) {
          t.bitrate = json["bitrate"].asInt64();
        }
        if (json.isMember("sample_rate")) {
          t.sample_rate = json["sample_rate"].asInt64();
        }
        if (json.isMember("channels")) {
          t.channels = json["channels"].asInt64();
        }
        if (json.isMember("format")) {
          t.format = json["format"].asString();
        }
        if (json.isMember("size_bytes")) {
          t.size_bytes = json["size_bytes"].asInt64();
        }
        if (json.isMember("mtime")) {
          t.mtime = json["mtime"].asInt64();
        }
        if (json.isMember("rating")) {
          t.rating = json["rating"].asInt64();
        }

        repo.updatePartial(t);
        return jsonResponse(200, "{\"status\":\"ok\"}");
      });

  std::cout << "[boot] 生效端口 主=" << cfg.port << " Jellyfin="
            << (cfg.jellyfin_enabled ? std::to_string(cfg.jellyfin_port)
                                     : std::string("off"))
            << " Subsonic="
            << (cfg.subsonic.enabled ? std::to_string(cfg.subsonic.port)
                                     : std::string("off"))
            << std::endl;

  // 三个服务共用进程内唯一的 Drogon app：入站按本地端口加路由前缀，
  // 各端口的路由互不串扰。
  http_server::installPortRouting();
  http_server::installErrorPages();
  if (cfg.jellyfin_enabled) {
    jellyfin::mount(cfg);
  }
  if (cfg.subsonic.enabled) {
    subsonic::mount(cfg);
  }

  http_server::listen(cfg.port);
  if (cfg.jellyfin_enabled) {
    http_server::listen(cfg.jellyfin_port);
  }
  if (cfg.subsonic.enabled) {
    http_server::listen(cfg.subsonic.port);
  }
  http_server::setWorkerThreads(8);
  http_server::setQuietLogging();

  drogon::app().run();

  storage_client::shutdown();
  media_library::shutdown_media_runtime();
  media_library::shutdown_library();
  image_cache::shutdown();
  return 0;
}
