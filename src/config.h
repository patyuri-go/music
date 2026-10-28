#ifndef CONFIG_H
#define CONFIG_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace config {
namespace fs = std::filesystem;

struct WebdavRoot {
  std::string url;
  std::string user;
  std::string pass;
};

struct ImageCacheConfig {
  bool enabled{true};
  fs::path dir{"./img_tmp"};
  std::uintmax_t max_bytes{2ull * 1024 * 1024 * 1024};
  std::uintmax_t max_file_bytes{20ull * 1024 * 1024};
  int ttl_days{30};
  int gc_interval_seconds{3600};
  int revalidate_seconds{300};
};

struct Config {
  fs::path db_path{"./music.db"};
  fs::path static_dir{"web"};
  int port{5666};
  int page_size{50};
  bool follow_symlinks{true};
  bool scan_on_start{true};
  int scan_threads{4};
  std::vector<fs::path> scan_roots{"/opt/music"};
  bool transcode_enabled{false};
  bool jellyfin_enabled{true};
  int jellyfin_port{9000};
  std::string jellyfin_server_name{"musicd"};
  bool webdav_enabled{false};
  std::vector<WebdavRoot> webdav_roots;
  fs::path playlist_dir{"./playlists"};
  ImageCacheConfig image_cache;
};

const Config &get();
const Config &load(const std::string &file = "config.json");

// 命令行覆盖：在 load() 之后调用，修改已加载的配置。
void setPort(int port);
void setJellyfinPort(int port);
void setJellyfinEnabled(bool enabled);

} // namespace config

#endif
