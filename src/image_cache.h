#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace image_cache {

struct Options {
  bool enabled = true;
  std::filesystem::path dir{"./img_tmp"};
  std::uintmax_t max_bytes = 2ull * 1024 * 1024 * 1024;
  std::uintmax_t max_file_bytes = 20ull * 1024 * 1024;
  int ttl_days = 30;
  int gc_interval_seconds = 3600;
  int revalidate_seconds = 300;
};

struct Stats {
  std::uint64_t entries = 0;
  std::uintmax_t bytes = 0;
};

class ImageCache {
public:
  explicit ImageCache(Options opt);
  ~ImageCache();
  ImageCache(const ImageCache &) = delete;
  ImageCache &operator=(const ImageCache &) = delete;

  bool init(std::string &error);
  void shutdown();
  bool enabled() const;

  std::optional<std::filesystem::path>
  ensure(const std::string &virtual_path, const std::string &variant,
         std::string &error);

  void gc();
  Stats stats() const;

private:
  struct SourceInfo {
    std::string source_sig;
    std::string ext;
    std::uintmax_t size = 0;
    bool is_local = false;
  };

  struct Row {
    std::filesystem::path path;
    bool is_local = false;
    std::string image_id;
    std::string source_sig;
    std::uintmax_t bytes = 0;
    std::int64_t last_hit_at = 0;
    std::int64_t next_validate_at = 0;
  };

  struct RevalTask {
    std::string virtual_path;
    std::string variant;
    std::string image_id;
    std::string source_sig;
    bool is_local = false;
    std::filesystem::path path;
  };

  bool isCacheable(const std::string &resolved_location) const;
  bool isLocalSource(const std::string &resolved_location) const;
  std::optional<SourceInfo>
  computeSourceSig(const std::string &virtual_path,
                   const std::string &resolved_location);
  std::filesystem::path cachePath(const std::string &image_id,
                                  const std::string &ext) const;

  std::optional<Row> lookup(const std::string &virtual_path,
                            const std::string &variant);
  void touch(const std::string &virtual_path, const std::string &variant,
             std::int64_t now);
  void upsert(const std::string &virtual_path, const std::string &variant,
              const std::string &image_id, const std::string &source_sig,
              const std::filesystem::path &cache_file, bool is_local,
              std::uintmax_t bytes, std::int64_t next_validate_at);
  void deleteRow(const std::string &virtual_path,
                 const std::string &variant);
  void deleteIfMatches(const std::string &virtual_path,
                       const std::string &variant,
                       const std::string &image_id,
                       const std::string &source_sig);
  void markValidated(const std::string &virtual_path,
                     const std::string &variant,
                     const std::string &image_id,
                     const std::string &source_sig,
                     std::int64_t next_at);

  bool downloadToTemp(const std::string &resolved_location,
                      const std::filesystem::path &tmp_path,
                      std::string &image_id, std::uintmax_t &bytes,
                      std::string &error);
  bool hashLocalFile(const std::filesystem::path &path, std::string &image_id,
                     std::uintmax_t &bytes, std::string &error);
  std::optional<std::filesystem::path>
  fetchAndStore(const std::string &virtual_path, const std::string &variant,
                const std::string &resolved_location, std::string &error);

  void scheduleRevalidation(const Row &row,
                            const std::string &virtual_path,
                            const std::string &variant, std::int64_t now);
  void revalidateLoop();
  bool revalidateOne(const RevalTask &task);

  void runGc();
  void evictRows();
  std::set<std::string> referencedFiles();
  void removeOrphans(const std::set<std::string> &referenced);
  void purgeMissingRows();
  void gcLoop();

  Options opt_;
  std::filesystem::path db_path_;
  std::atomic<bool> enabled_{false};
  std::atomic<bool> stopping_{false};

  mutable std::mutex db_write_mu_;
  std::mutex commit_mu_;
  std::mutex inflight_mu_;
  std::unordered_map<std::string,
                     std::shared_future<std::optional<std::filesystem::path>>>
      inflight_;

  std::mutex reval_mu_;
  std::condition_variable reval_cv_;
  std::deque<RevalTask> reval_queue_;
  std::unordered_set<std::string> reval_pending_;
  std::unordered_map<std::string, int> reval_fail_;
  std::thread reval_thread_;

  std::mutex gc_mu_;
  std::condition_variable gc_cv_;
  std::thread gc_thread_;
};

bool init(const Options &opt, std::string &error);
void shutdown();
std::shared_ptr<ImageCache> get();

} // namespace image_cache
