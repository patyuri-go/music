#ifndef CONFIG_H
#define CONFIG_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace config {
namespace fs = std::filesystem;

struct ImageCacheConfig {
  bool enabled{true};
  fs::path dir{"./img_tmp"};
  std::uintmax_t max_bytes{2ull * 1024 * 1024 * 1024};
  std::uintmax_t max_file_bytes{20ull * 1024 * 1024};
  int ttl_days{30};
  int gc_interval_seconds{3600};
  int revalidate_seconds{300};
};

// 本地媒体库：全局只允许一个，id 可配（默认 local-main）。
// 扫描根（含 WebDAV 凭据）不进配置文件，存在数据库的 scan_roots 表里。
struct LocalLibraryConfig {
  std::string id{"local-main"};
  bool follow_symlinks{true};
  bool scan_on_start{true};
  int scan_threads{4};
  fs::path playlist_dir{"./playlists"};
};

// 网易云音乐（经 api-enhanced 适配）。
// cookie 是明文凭据：只在这里与请求头发送路径上存在，绝不进日志/错误/响应。
// id 目前必须是 netease-main：实体引用由 api-enhanced 的常量生成。
struct NeteaseConfig {
  std::string id{"netease-main"};
  bool enabled{false};
  std::string api_base{"http://127.0.0.1:3000"};
  std::string cookie;
  // standard | higher | exhigh | lossless | hires
  std::string default_level{"exhigh"};
  // 网易云用户 id，用于列出「我的歌单」；留空则 list_playlists 返回 InvalidRequest。
  std::string uid;
  int timeout_ms{8000};
  int metadata_ttl_ms{600000};
  // 同时在飞的 HTTP 请求数上限。注意它同时被两层使用：
  //   * 库级准入（注册表按库操作计数）；
  //   * 传输层闸门（按真实 HTTP 请求计数）。
  int max_concurrency{4};
  // 同一库任意两次 HTTP 请求「发起时刻」的最小间隔；0 表示不限。
  // 只作用于真实请求：client 的 TTL 缓存命中不会占间隔。
  int min_request_interval_ms{0};
  // 可选：透传 realIP，规避地域风控。
  std::string real_ip;
};

// Subsonic / Navidrome 兼容门面（供第三方客户端使用）。
// 独立端口运行，与 Jellyfin 门面平行；不做账号校验。
struct SubsonicConfig {
  bool enabled{false};
  int port{4533};
  std::string server_name{"musicd"};
  int default_page_size{50};
  // 每请求一行访问日志；凭据类参数（u/t/s/p）永不打印。
  bool log_requests{true};
  // CORS 允许的来源：`*` 回显任意 Origin、`a,b` 白名单、空串关闭。
  // 浏览器里的 Web 客户端是跨源访问，缺这个头会表现成 "Failed to fetch"。
  std::string cors_allow_origin{"*"};
};

// 单个方面的媒体库优先顺序。aspect：metadata / lyrics / stream / artwork / search。
// search 控制多库聚合列表（搜索结果等）的分层顺序。
struct LibraryPriorityConfig {
  std::string aspect;
  std::vector<std::string> library_ids;
};

// 本地 SQLite 主库。
struct DatabaseSqlConfig {
  fs::path path{"./music.db"};
};

// 远端主库：直接连外部提供的 PostgreSQL 服务（SOCI + 系统 libpq）。
//
// `url` 是 libpq 连接串，两种写法都接受：
//   1. URI：`postgresql://[user[:pass]@]host[:port][/dbname][?k=v...]`
//   2. 关键字：`host=127.0.0.1 port=5432 user=musicd`
// `password` / `dbname` 会追加到 `url` 上（URI 走查询参数，关键字形式直接追加）。
// `url` 为空表示不启用 PostgreSQL，主库回退到 `sql`。
struct DatabasePostgresConfig {
  std::string url;
  std::string user{"musicd"};
  std::string password;
  std::string dbname{"music"};
  // password 为空时回退读取该环境变量；两者都空则由服务端认证失败报错。
  std::string password_env{"MUSICD_DB_PASSWORD"};
  // 追加到连接串的 sslmode；空表示不追加。取值见 libpq 文档。
  std::string sslmode;
  int connect_timeout_seconds{5};
};

// 主库配置。同一二进制同时包含 sqlite 与 postgres 两种实现；
// `postgres.url` 非空即走 PostgreSQL，否则走本地 SQLite。
struct DatabaseConfig {
  DatabaseSqlConfig sql;
  DatabasePostgresConfig postgres;

  bool usePostgres() const { return !postgres.url.empty(); }
};

struct Config {
  fs::path db_path{"./music.db"};
  // 新的后端配置；未配置 database 段时 sqlite_path 继承 db_path。
  DatabaseConfig database;
  fs::path static_dir{"web"};
  // 主服务端口（配置文件键名 main_port）。
  int port{5666};
  int page_size{50};
  bool transcode_enabled{false};
  bool jellyfin_enabled{true};
  int jellyfin_port{9000};
  std::string jellyfin_server_name{"musicd"};
  SubsonicConfig subsonic;
  ImageCacheConfig image_cache;
  LocalLibraryConfig local;
  NeteaseConfig netease;
  // 每个方面一条顺序；数组顺序无意义。未配置的方面走内建顺序。
  std::vector<LibraryPriorityConfig> library_priority;
};

const Config &get();
const Config &load(const std::string &file = "config.json");

// 命令行覆盖：在 load() 之后调用，修改已加载的配置。
void setPort(int port);
void setJellyfinPort(int port);
void setJellyfinEnabled(bool enabled);

// 网易云命令行覆盖。优先级：CLI > config.json > 环境变量 > 默认值。
void setNeteaseEnabled(bool enabled);
void setNeteaseApi(std::string api_base);
void setNeteaseCookie(std::string cookie);
void setNeteaseUid(std::string uid);

} // namespace config

#endif
