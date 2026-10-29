#include "image_cache.h"

#include "api/webdav/webdav.h"
#include "config.h"
#include "database.h"
#include "storage.h"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>
#include <SQLiteCpp/Transaction.h>

#include <openssl/evp.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <sys/stat.h>
#include <unordered_map>

namespace image_cache {
namespace {

namespace fs = std::filesystem;

using Clock = std::chrono::system_clock;

std::int64_t nowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             Clock::now().time_since_epoch())
      .count();
}

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return value;
}

std::string hexString(const unsigned char *data, std::size_t size) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0x0f]);
  }
  return out;
}

class ContentHasher {
public:
  ContentHasher() : ctx_(EVP_MD_CTX_new(), &EVP_MD_CTX_free) {
    if (ctx_) {
      ok_ = EVP_DigestInit_ex(ctx_.get(), EVP_sha256(), nullptr) == 1;
    }
  }
  ContentHasher(const ContentHasher &) = delete;
  ContentHasher &operator=(const ContentHasher &) = delete;

  void update(const void *data, std::size_t size) {
    if (ok_ && size > 0) {
      ok_ = EVP_DigestUpdate(ctx_.get(), data, size) == 1;
    }
  }

  std::optional<std::string> finalHex() {
    if (!ok_ || !ctx_) {
      return std::nullopt;
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(ctx_.get(), digest, &length) != 1) {
      return std::nullopt;
    }
    return hexString(digest, length);
  }

private:
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx_;
  bool ok_ = false;
};

bool knownExtension(const std::string &ext) {
  static const char *kKnown[] = {"jpg",  "jpeg", "png", "webp",
                                 "gif",  "bmp",  "avif"};
  const std::string lower = toLower(ext);
  for (const char *value : kKnown) {
    if (lower == value) {
      return true;
    }
  }
  return false;
}

std::string extensionOf(const fs::path &path) {
  std::string ext = toLower(path.extension().string());
  if (!ext.empty() && ext.front() == '.') {
    ext.erase(ext.begin());
  }
  return knownExtension(ext) ? ext : std::string{};
}

std::string extensionOfContentType(const std::string &content_type) {
  const std::string type = toLower(content_type);
  if (type.find("image/jpeg") != std::string::npos) {
    return "jpg";
  }
  if (type.find("image/png") != std::string::npos) {
    return "png";
  }
  if (type.find("image/webp") != std::string::npos) {
    return "webp";
  }
  if (type.find("image/gif") != std::string::npos) {
    return "gif";
  }
  if (type.find("image/bmp") != std::string::npos) {
    return "bmp";
  }
  if (type.find("image/avif") != std::string::npos) {
    return "avif";
  }
  return {};
}

std::string randomTempName() {
  std::random_device rd;
  std::uint64_t high = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
  std::uint64_t low = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
  unsigned char bytes[16];
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<unsigned char>((high >> (i * 8)) & 0xff);
    bytes[8 + i] = static_cast<unsigned char>((low >> (i * 8)) & 0xff);
  }
  return hexString(bytes, sizeof(bytes));
}

std::string slotKey(const std::string &virtual_path,
                    const std::string &variant) {
  return virtual_path + '\x1f' + variant;
}

bool underAnyRoot(const fs::path &target) {
  std::error_code ec;
  const fs::path canonical = fs::weakly_canonical(target, ec);
  if (ec) {
    return false;
  }
  repo::ScanRootRepo roots(config::get().db_path.string());
  for (const auto &root : roots.list()) {
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

SQLite::Database &openDb(const fs::path &path) {
  static thread_local std::unordered_map<std::string,
                                        std::unique_ptr<SQLite::Database>>
      connections;
  const std::string key = path.string();
  const auto it = connections.find(key);
  if (it != connections.end()) {
    return *it->second;
  }
  auto database = std::make_unique<SQLite::Database>(
      key, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 5000);
  database->exec("PRAGMA journal_mode=WAL");
  database->exec("PRAGMA synchronous=NORMAL");
  database->exec("PRAGMA busy_timeout=5000");
  SQLite::Database &result = *database;
  connections.emplace(key, std::move(database));
  return result;
}

void ensureSchema(SQLite::Database &database) {
  database.exec("CREATE TABLE IF NOT EXISTS image_cache ("
                "virtual_path TEXT NOT NULL,"
                "variant TEXT NOT NULL DEFAULT 'original',"
                "image_id TEXT NOT NULL,"
                "source_sig TEXT NOT NULL,"
                "last_hit_at INTEGER NOT NULL,"
                "cache_file TEXT NOT NULL,"
                "bytes INTEGER NOT NULL,"
                "created_at INTEGER NOT NULL,"
                "next_validate_at INTEGER NOT NULL DEFAULT 0,"
                "is_local INTEGER NOT NULL DEFAULT 0,"
                "PRIMARY KEY (virtual_path, variant))");
  database.exec("CREATE INDEX IF NOT EXISTS idx_image_cache_id "
                "ON image_cache(image_id)");
  database.exec("CREATE INDEX IF NOT EXISTS idx_image_cache_hit "
                "ON image_cache(last_hit_at)");
  const int version =
      database.execAndGet("PRAGMA user_version").getInt();
  if (version == 0) {
    database.exec("PRAGMA user_version=1");
  }
}

} // namespace

ImageCache::ImageCache(Options opt) : opt_(std::move(opt)) {}

ImageCache::~ImageCache() { shutdown(); }

bool ImageCache::init(std::string &error) {
  if (enabled_) {
    return true;
  }
  if (!opt_.enabled) {
    enabled_ = false;
    return true;
  }

  auto fail = [&](const std::string &message) {
    error = message;
    return false;
  };

  std::error_code ec;
  const fs::path absolute_dir = fs::absolute(opt_.dir, ec);
  if (!ec) {
    opt_.dir = absolute_dir;
  }
  fs::create_directories(opt_.dir, ec);
  if (ec) {
    return fail("cannot create image cache directory: " + ec.message());
  }
  const fs::path tmp_dir = opt_.dir / "tmp";
  fs::remove_all(tmp_dir, ec);
  ec.clear();
  fs::create_directories(tmp_dir, ec);
  if (ec) {
    return fail("cannot create image cache tmp directory: " + ec.message());
  }

  db_path_ = opt_.dir / "index.db";
  try {
    SQLite::Database &database = openDb(db_path_);
    ensureSchema(database);
    purgeMissingRows();
  } catch (const std::exception &ex) {
    return fail(std::string("cannot open image cache database: ") + ex.what());
  }

  stopping_ = false;
  enabled_ = true;
  gc_thread_ = std::thread([this]() { gcLoop(); });
  reval_thread_ = std::thread([this]() { revalidateLoop(); });
  return true;
}

void ImageCache::shutdown() {
  if (stopping_.exchange(true)) {
    return;
  }
  enabled_ = false;
  reval_cv_.notify_all();
  gc_cv_.notify_all();
  if (reval_thread_.joinable()) {
    reval_thread_.join();
  }
  if (gc_thread_.joinable()) {
    gc_thread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(reval_mu_);
    reval_queue_.clear();
    reval_pending_.clear();
  }
}

bool ImageCache::enabled() const { return enabled_ && !stopping_; }

std::optional<fs::path>
ImageCache::ensure(const std::string &virtual_path, const std::string &variant,
                   std::string &error) {
  if (!enabled()) {
    error = "image cache is disabled";
    return std::nullopt;
  }
  if (variant != "original") {
    error = "unsupported image variant: " + variant;
    return std::nullopt;
  }
  if (virtual_path.empty()) {
    error = "empty virtual path";
    return std::nullopt;
  }

  try {
    const std::int64_t now = nowSeconds();
    const auto hit = lookup(virtual_path, variant);
    if (hit) {
      std::error_code ec;
      const std::uintmax_t size = fs::file_size(hit->path, ec);
      if (!ec && size == hit->bytes) {
        if (now >= hit->next_validate_at) {
          scheduleRevalidation(*hit, virtual_path, variant, now);
        }
        if (now - hit->last_hit_at > 60) {
          touch(virtual_path, variant, now);
        }
        return hit->path;
      }
      deleteRow(virtual_path, variant);
    }

    const std::string key = slotKey(virtual_path, variant);
    std::shared_future<std::optional<fs::path>> waiter;
    std::promise<std::optional<fs::path>> leader_promise;
    bool leader = false;
    {
      std::lock_guard<std::mutex> lock(inflight_mu_);
      const auto it = inflight_.find(key);
      if (it != inflight_.end()) {
        waiter = it->second;
      } else {
        leader_promise = std::promise<std::optional<fs::path>>();
        waiter = leader_promise.get_future().share();
        inflight_.emplace(key, waiter);
        leader = true;
      }
    }

    if (!leader) {
      const auto result = waiter.get();
      if (!result) {
        error = "image cache fetch failed";
      }
      return result;
    }

    std::optional<fs::path> result;
    std::string failure;
    try {
      const auto second = lookup(virtual_path, variant);
      if (second) {
        std::error_code ec;
        if (fs::file_size(second->path, ec) == second->bytes && !ec) {
          result = second->path;
        } else {
          deleteRow(virtual_path, variant);
        }
      }
      if (!result) {
        const std::string resolved = storage::resolve(virtual_path);
        result = fetchAndStore(virtual_path, variant, resolved, failure);
      }
    } catch (const std::exception &ex) {
      failure = ex.what();
      result = std::nullopt;
    }

    leader_promise.set_value(result);
    {
      std::lock_guard<std::mutex> lock(inflight_mu_);
      inflight_.erase(key);
    }
    if (!result) {
      error = failure.empty() ? "image cache fetch failed" : failure;
    }
    return result;
  } catch (const std::exception &ex) {
    error = ex.what();
    return std::nullopt;
  }
}

bool ImageCache::isLocalSource(const std::string &resolved_location) const {
  return !webdav::isWebdavUrl(resolved_location);
}

bool ImageCache::isCacheable(const std::string &resolved_location) const {
  if (webdav::isWebdavUrl(resolved_location)) {
    return true;
  }
  std::error_code ec;
  const fs::path path(resolved_location);
  if (!fs::is_regular_file(path, ec) || ec) {
    return false;
  }
  return underAnyRoot(path);
}

std::optional<ImageCache::SourceInfo>
ImageCache::computeSourceSig(const std::string &virtual_path,
                             const std::string &resolved_location) {
  if (webdav::isWebdavUrl(resolved_location)) {
    const webdav::Url url = webdav::resolveCredentials(resolved_location);
    if (url.host.empty()) {
      return std::nullopt;
    }
    webdav::Client client(url);
    const auto metadata = client.probe(url.path);
    if (!metadata) {
      return std::nullopt;
    }
    const std::string length = metadata->length >= 0
                                   ? std::to_string(metadata->length)
                                   : std::string("-1");
    const std::string last_modified = metadata->last_modified;
    const std::string content_type = metadata->content_type;
    std::string ext = extensionOfContentType(content_type);
    if (ext.empty()) {
      ext = extensionOf(fs::path(url.path));
    }
    if (ext.empty()) {
      ext = "img";
    }

    SourceInfo info;
    info.source_sig = "v1|webdav|" + virtual_path + "|" + length + "|" +
                      last_modified + "|" + content_type;
    info.ext = ext;
    try {
      info.size = static_cast<std::uintmax_t>(std::stoull(length));
    } catch (const std::exception &) {
      info.size = 0;
    }
    info.is_local = false;
    return info;
  }

  struct stat statbuf {};
  if (::stat(resolved_location.c_str(), &statbuf) != 0 ||
      !S_ISREG(statbuf.st_mode)) {
    return std::nullopt;
  }
  std::string ext = extensionOf(fs::path(resolved_location));
  if (ext.empty()) {
    ext = "img";
  }
  std::int64_t mtime_ns = 0;
#if defined(__linux__)
  mtime_ns = static_cast<std::int64_t>(statbuf.st_mtim.tv_sec) * 1000000000LL +
             static_cast<std::int64_t>(statbuf.st_mtim.tv_nsec);
#else
  mtime_ns = static_cast<std::int64_t>(statbuf.st_mtime) * 1000000000LL;
#endif
  SourceInfo info;
  info.source_sig = "v1|local|" + virtual_path + "|" +
                    std::to_string(static_cast<std::uintmax_t>(statbuf.st_size)) +
                    "|" + std::to_string(mtime_ns) + "|" + ext;
  info.ext = ext;
  info.size = static_cast<std::uintmax_t>(statbuf.st_size);
  info.is_local = true;
  return info;
}

fs::path ImageCache::cachePath(const std::string &image_id,
                               const std::string &ext) const {
  std::string suffix = toLower(ext);
  if (!suffix.empty() && suffix.front() == '.') {
    suffix.erase(suffix.begin());
  }
  if (!knownExtension(suffix)) {
    suffix = "img";
  }
  if (image_id.size() < 4) {
    return opt_.dir / "tmp" / (image_id + "." + suffix);
  }
  return opt_.dir / image_id.substr(0, 2) / image_id.substr(2, 2) /
         (image_id + "." + suffix);
}

std::optional<ImageCache::Row>
ImageCache::lookup(const std::string &virtual_path,
                   const std::string &variant) {
  SQLite::Database &database = openDb(db_path_);
  SQLite::Statement query(
      database,
      "SELECT image_id, source_sig, cache_file, bytes, last_hit_at, "
      "next_validate_at, is_local FROM image_cache "
      "WHERE virtual_path=? AND variant=?");
  query.bind(1, virtual_path);
  query.bind(2, variant);
  if (!query.executeStep()) {
    return std::nullopt;
  }
  Row row;
  row.image_id = query.getColumn(0).getString();
  row.source_sig = query.getColumn(1).getString();
  const std::string cache_file = query.getColumn(2).getString();
  row.bytes = static_cast<std::uintmax_t>(query.getColumn(3).getInt64());
  row.last_hit_at = query.getColumn(4).getInt64();
  row.next_validate_at = query.getColumn(5).getInt64();
  row.is_local = query.getColumn(6).getInt() != 0;
  row.path = row.is_local ? fs::path(cache_file) : (opt_.dir / cache_file);
  return row;
}

void ImageCache::touch(const std::string &virtual_path,
                       const std::string &variant, std::int64_t now) {
  std::lock_guard<std::mutex> lock(db_write_mu_);
  SQLite::Database &database = openDb(db_path_);
  SQLite::Statement update(
      database,
      "UPDATE image_cache SET last_hit_at=? "
      "WHERE virtual_path=? AND variant=?");
  update.bind(1, now);
  update.bind(2, virtual_path);
  update.bind(3, variant);
  update.exec();
}

void ImageCache::upsert(const std::string &virtual_path,
                        const std::string &variant,
                        const std::string &image_id,
                        const std::string &source_sig,
                        const fs::path &cache_file, bool is_local,
                        std::uintmax_t bytes, std::int64_t next_validate_at) {
  std::lock_guard<std::mutex> lock(db_write_mu_);
  SQLite::Database &database = openDb(db_path_);
  SQLite::Statement insert(
      database,
      "INSERT INTO image_cache(virtual_path, variant, image_id, source_sig, "
      "last_hit_at, cache_file, bytes, created_at, next_validate_at, is_local) "
      "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(virtual_path, variant) DO UPDATE SET "
      "image_id=excluded.image_id, source_sig=excluded.source_sig, "
      "last_hit_at=excluded.last_hit_at, cache_file=excluded.cache_file, "
      "bytes=excluded.bytes, next_validate_at=excluded.next_validate_at, "
      "is_local=excluded.is_local");
  const std::int64_t now = nowSeconds();
  insert.bind(1, virtual_path);
  insert.bind(2, variant);
  insert.bind(3, image_id);
  insert.bind(4, source_sig);
  insert.bind(5, now);
  insert.bind(6, cache_file.string());
  insert.bind(7, static_cast<std::int64_t>(bytes));
  insert.bind(8, now);
  insert.bind(9, next_validate_at);
  insert.bind(10, is_local ? 1 : 0);
  insert.exec();
}

void ImageCache::deleteRow(const std::string &virtual_path,
                           const std::string &variant) {
  std::lock_guard<std::mutex> lock(db_write_mu_);
  SQLite::Database &database = openDb(db_path_);
  SQLite::Statement remove(
      database,
      "DELETE FROM image_cache WHERE virtual_path=? AND variant=?");
  remove.bind(1, virtual_path);
  remove.bind(2, variant);
  remove.exec();
}

void ImageCache::deleteIfMatches(const std::string &virtual_path,
                                 const std::string &variant,
                                 const std::string &image_id,
                                 const std::string &source_sig) {
  std::lock_guard<std::mutex> lock(db_write_mu_);
  SQLite::Database &database = openDb(db_path_);
  SQLite::Statement remove(
      database,
      "DELETE FROM image_cache WHERE virtual_path=? AND variant=? "
      "AND image_id=? AND source_sig=?");
  remove.bind(1, virtual_path);
  remove.bind(2, variant);
  remove.bind(3, image_id);
  remove.bind(4, source_sig);
  remove.exec();
}

void ImageCache::markValidated(const std::string &virtual_path,
                               const std::string &variant,
                               const std::string &image_id,
                               const std::string &source_sig,
                               std::int64_t next_at) {
  std::lock_guard<std::mutex> lock(db_write_mu_);
  SQLite::Database &database = openDb(db_path_);
  SQLite::Statement update(
      database,
      "UPDATE image_cache SET next_validate_at=? "
      "WHERE virtual_path=? AND variant=? AND image_id=? AND source_sig=?");
  update.bind(1, next_at);
  update.bind(2, virtual_path);
  update.bind(3, variant);
  update.bind(4, image_id);
  update.bind(5, source_sig);
  update.exec();
}

bool ImageCache::downloadToTemp(const std::string &resolved_location,
                                const fs::path &tmp_path,
                                std::string &image_id, std::uintmax_t &bytes,
                                std::string &error) {
  const webdav::Url url = webdav::resolveCredentials(resolved_location);
  if (url.host.empty()) {
    error = "invalid webdav url";
    return false;
  }
  std::cerr << "[image_cache] webdav fetch " << resolved_location << '\n';
  webdav::Stream stream = webdav::openStream(url, url.path, {});
  if (stream.status != 200 && stream.status != 206) {
    stream.close();
    error = "webdav image request failed: " + std::to_string(stream.status);
    return false;
  }
  if (stream.length >= 0 &&
      static_cast<std::uintmax_t>(stream.length) > opt_.max_file_bytes) {
    stream.close();
    error = "image exceeds max_file_bytes";
    return false;
  }

  std::ofstream output(tmp_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    stream.close();
    error = "cannot create temporary cache file";
    return false;
  }

  ContentHasher hasher;
  std::array<char, 64 * 1024> buffer{};
  std::uintmax_t total = 0;
  bool ok = true;
  while (true) {
    if (stream.length >= 0 &&
        total >= static_cast<std::uintmax_t>(stream.length)) {
      break;
    }
    const long long count = stream.read(buffer.data(), buffer.size());
    if (count < 0) {
      if (stream.length >= 0 &&
          total == static_cast<std::uintmax_t>(stream.length)) {
        break;
      }
      ok = false;
      error = "webdav image read failed";
      break;
    }
    if (count == 0) {
      break;
    }
    total += static_cast<std::uintmax_t>(count);
    if (opt_.max_file_bytes > 0 && total > opt_.max_file_bytes) {
      ok = false;
      error = "image exceeds max_file_bytes";
      break;
    }
    output.write(buffer.data(), static_cast<std::streamsize>(count));
    if (!output) {
      ok = false;
      error = "cannot write temporary cache file";
      break;
    }
    hasher.update(buffer.data(), static_cast<std::size_t>(count));
  }
  stream.close();
  output.close();

  if (!ok) {
    std::error_code ec;
    fs::remove(tmp_path, ec);
    return false;
  }
  const std::optional<std::string> digest = hasher.finalHex();
  if (!digest) {
    std::error_code ec;
    fs::remove(tmp_path, ec);
    error = "cannot hash downloaded image";
    return false;
  }
  image_id = *digest;
  bytes = total;
  return true;
}

bool ImageCache::hashLocalFile(const fs::path &path, std::string &image_id,
                               std::uintmax_t &bytes, std::string &error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "cannot open local image";
    return false;
  }
  ContentHasher hasher;
  std::array<char, 64 * 1024> buffer{};
  std::uintmax_t total = 0;
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      hasher.update(buffer.data(), static_cast<std::size_t>(count));
      total += static_cast<std::uintmax_t>(count);
    }
  }
  if (input.bad()) {
    error = "cannot read local image";
    return false;
  }
  const std::optional<std::string> digest = hasher.finalHex();
  if (!digest) {
    error = "cannot hash local image";
    return false;
  }
  image_id = *digest;
  bytes = total;
  return true;
}

std::optional<fs::path>
ImageCache::fetchAndStore(const std::string &virtual_path,
                          const std::string &variant,
                          const std::string &resolved_location,
                          std::string &error) {
  if (variant != "original") {
    error = "unsupported image variant: " + variant;
    return std::nullopt;
  }

  const auto source = computeSourceSig(virtual_path, resolved_location);
  if (!source) {
    error = "cannot read image source metadata";
    return std::nullopt;
  }
  if (!isCacheable(resolved_location)) {
    error = "image source is not cacheable";
    return std::nullopt;
  }

  const std::int64_t next_validate_at =
      nowSeconds() + std::max(0, opt_.revalidate_seconds);

  if (source->is_local) {
    std::error_code ec;
    const fs::path source_path = fs::weakly_canonical(resolved_location, ec);
    if (ec) {
      error = "cannot canonicalize local image";
      return std::nullopt;
    }
    std::string image_id;
    std::uintmax_t bytes = 0;
    if (!hashLocalFile(source_path, image_id, bytes, error)) {
      return std::nullopt;
    }
    try {
      upsert(virtual_path, variant, image_id, source->source_sig,
             source_path, true, bytes, next_validate_at);
    } catch (const std::exception &ex) {
      std::cerr << "[image_cache] failed to write local index row: "
                << ex.what() << '\n';
    }
    return source_path;
  }

  if (opt_.max_file_bytes > 0 && source->size > opt_.max_file_bytes) {
    error = "image exceeds max_file_bytes";
    return std::nullopt;
  }

  const fs::path tmp_path = opt_.dir / "tmp" / (randomTempName() + ".part");
  std::string image_id;
  std::uintmax_t bytes = 0;
  if (!downloadToTemp(resolved_location, tmp_path, image_id, bytes, error)) {
    return std::nullopt;
  }

  std::lock_guard<std::mutex> commit_lock(commit_mu_);
  const fs::path target = cachePath(image_id, source->ext);
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (ec) {
    fs::remove(tmp_path, ec);
    error = "cannot create image cache shard directory";
    return std::nullopt;
  }

  if (fs::exists(target, ec)) {
    fs::remove(tmp_path, ec);
  } else {
    fs::rename(tmp_path, target, ec);
    if (ec) {
      fs::remove(tmp_path, ec);
      error = "cannot move temporary image into cache";
      return std::nullopt;
    }
  }

  std::error_code relative_ec;
  const fs::path relative = fs::relative(target, opt_.dir, relative_ec);
  const std::string cache_file =
      relative_ec ? target.string() : relative.generic_string();
  try {
    upsert(virtual_path, variant, image_id, source->source_sig, cache_file,
           false, bytes, next_validate_at);
  } catch (const std::exception &ex) {
    std::cerr << "[image_cache] failed to write index row: " << ex.what()
              << '\n';
  }
  return target;
}

void ImageCache::scheduleRevalidation(const Row &row,
                                      const std::string &virtual_path,
                                      const std::string &variant,
                                      std::int64_t now) {
  if (opt_.revalidate_seconds <= 0) {
    return;
  }
  RevalTask task;
  task.virtual_path = virtual_path;
  task.variant = variant;
  task.image_id = row.image_id;
  task.source_sig = row.source_sig;
  task.is_local = row.is_local;
  task.path = row.path;

  const std::string key = slotKey(virtual_path, variant);
  {
    std::lock_guard<std::mutex> lock(reval_mu_);
    if (stopping_ || reval_pending_.find(key) != reval_pending_.end() ||
        reval_queue_.size() >= 4096) {
      return;
    }
    reval_pending_.insert(key);
    reval_queue_.push_back(std::move(task));
  }
  reval_cv_.notify_one();
  markValidated(virtual_path, variant, row.image_id, row.source_sig,
                now + opt_.revalidate_seconds);
}

void ImageCache::revalidateLoop() {
  while (true) {
    RevalTask task;
    {
      std::unique_lock<std::mutex> lock(reval_mu_);
      reval_cv_.wait(lock, [this]() {
        return stopping_.load() || !reval_queue_.empty();
      });
      if (stopping_) {
        break;
      }
      task = std::move(reval_queue_.front());
      reval_queue_.pop_front();
    }

    bool ok = false;
    try {
      ok = revalidateOne(task);
    } catch (const std::exception &ex) {
      std::cerr << "[image_cache] revalidation failed: " << ex.what() << '\n';
      ok = false;
    }

    const std::string key = slotKey(task.virtual_path, task.variant);
    std::int64_t retry_delay = 0;
    {
      std::lock_guard<std::mutex> lock(reval_mu_);
      reval_pending_.erase(key);
      if (ok) {
        reval_fail_.erase(key);
      } else if (!stopping_) {
        int failures = 0;
        const auto it = reval_fail_.find(key);
        if (it != reval_fail_.end()) {
          failures = it->second;
        } else if (reval_fail_.size() < 1024) {
          reval_fail_.emplace(key, 0);
        } else {
          failures = 0;
        }
        failures = std::min(failures + 1, 10);
        if (reval_fail_.size() < 1024 ||
            reval_fail_.find(key) != reval_fail_.end()) {
          reval_fail_[key] = failures;
        }
        std::int64_t delay = std::max(1, opt_.revalidate_seconds);
        for (int i = 0; i < failures && delay < 3600; ++i) {
          delay *= 2;
        }
        retry_delay = std::min<std::int64_t>(delay, 3600);
      }
    }
    if (retry_delay > 0) {
      try {
        markValidated(task.virtual_path, task.variant, task.image_id,
                      task.source_sig, nowSeconds() + retry_delay);
      } catch (const std::exception &) {
      }
    }
  }
}

bool ImageCache::revalidateOne(const RevalTask &task) {
  if (task.variant != "original" || !enabled()) {
    return true;
  }
  const std::string resolved = storage::resolve(task.virtual_path);
  const bool local = !webdav::isWebdavUrl(resolved);
  if (local && !underAnyRoot(fs::path(resolved))) {
    deleteIfMatches(task.virtual_path, task.variant, task.image_id,
                    task.source_sig);
    return true;
  }
  if (task.is_local) {
    if (!local) {
      deleteIfMatches(task.virtual_path, task.variant, task.image_id,
                      task.source_sig);
      return true;
    }
    std::error_code ec1;
    std::error_code ec2;
    const fs::path current = fs::weakly_canonical(resolved, ec1);
    const fs::path stored = fs::weakly_canonical(task.path, ec2);
    if (ec1 || ec2 || current != stored) {
      deleteIfMatches(task.virtual_path, task.variant, task.image_id,
                      task.source_sig);
      return true;
    }
  }

  const auto source = computeSourceSig(task.virtual_path, resolved);
  if (!source) {
    return false;
  }
  if (source->source_sig != task.source_sig) {
    deleteIfMatches(task.virtual_path, task.variant, task.image_id,
                    task.source_sig);
    return true;
  }
  markValidated(task.virtual_path, task.variant, task.image_id,
                task.source_sig, nowSeconds() + opt_.revalidate_seconds);
  return true;
}

void ImageCache::gc() {
  std::lock_guard<std::mutex> lock(gc_mu_);
  runGc();
}

void ImageCache::runGc() {
  try {
    SQLite::Database &database = openDb(db_path_);
    if (opt_.ttl_days > 0) {
      const std::int64_t cutoff =
          nowSeconds() - static_cast<std::int64_t>(opt_.ttl_days) * 86400;
      std::lock_guard<std::mutex> lock(db_write_mu_);
      SQLite::Statement remove(
          database, "DELETE FROM image_cache WHERE last_hit_at < ?");
      remove.bind(1, cutoff);
      remove.exec();
    }
  } catch (const std::exception &ex) {
    std::cerr << "[image_cache] TTL cleanup failed: " << ex.what() << '\n';
  }

  try {
    evictRows();
  } catch (const std::exception &ex) {
    std::cerr << "[image_cache] capacity cleanup failed: " << ex.what()
              << '\n';
  }

  try {
    std::lock_guard<std::mutex> commit_lock(commit_mu_);
    const std::set<std::string> referenced = referencedFiles();
    removeOrphans(referenced);
  } catch (const std::exception &ex) {
    std::cerr << "[image_cache] orphan cleanup failed: " << ex.what() << '\n';
  }

  std::error_code ec;
  const fs::path tmp_dir = opt_.dir / "tmp";
  const auto cutoff = fs::file_time_type::clock::now() -
                      std::chrono::hours(1);
  for (fs::directory_iterator it(tmp_dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    std::error_code file_ec;
    if (!it->is_regular_file(file_ec)) {
      continue;
    }
    const auto modified = fs::last_write_time(it->path(), file_ec);
    if (!file_ec && modified < cutoff) {
      fs::remove(it->path(), file_ec);
    }
  }
}

void ImageCache::evictRows() {
  SQLite::Database &database = openDb(db_path_);
  struct Group {
    std::string cache_file;
    std::uintmax_t bytes = 0;
    std::int64_t last_hit = 0;
  };
  std::vector<Group> groups;
  std::uintmax_t total = 0;
  {
    SQLite::Statement query(
        database,
        "SELECT cache_file, MAX(bytes), MAX(last_hit_at) FROM image_cache "
        "WHERE is_local=0 GROUP BY cache_file "
        "ORDER BY MAX(last_hit_at) ASC");
    while (query.executeStep()) {
      Group group;
      group.cache_file = query.getColumn(0).getString();
      group.bytes =
          static_cast<std::uintmax_t>(query.getColumn(1).getInt64());
      group.last_hit = query.getColumn(2).getInt64();
      total += group.bytes;
      groups.push_back(std::move(group));
    }
  }
  if (opt_.max_bytes == 0 || total <= opt_.max_bytes) {
    return;
  }

  const std::uintmax_t target = (opt_.max_bytes / 10) * 9;
  std::lock_guard<std::mutex> lock(db_write_mu_);
  SQLite::Transaction transaction(database);
  for (const Group &group : groups) {
    if (total <= target) {
      break;
    }
    SQLite::Statement remove(
        database,
        "DELETE FROM image_cache WHERE cache_file=? AND is_local=0");
    remove.bind(1, group.cache_file);
    remove.exec();
    total = group.bytes >= total ? 0 : total - group.bytes;
  }
  transaction.commit();
}

std::set<std::string> ImageCache::referencedFiles() {
  std::set<std::string> files;
  SQLite::Database &database = openDb(db_path_);
  SQLite::Statement query(
      database, "SELECT DISTINCT cache_file FROM image_cache WHERE is_local=0");
  while (query.executeStep()) {
    files.insert(query.getColumn(0).getString());
  }
  return files;
}

void ImageCache::removeOrphans(const std::set<std::string> &referenced) {
  std::error_code ec;
  fs::recursive_directory_iterator it(opt_.dir, ec);
  const fs::recursive_directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    const fs::path path = it->path();
    const fs::path relative = fs::relative(path, opt_.dir, entry_ec);
    if (entry_ec) {
      continue;
    }
    const std::string name = relative.generic_string();
    if (it->is_directory(entry_ec)) {
      if (name == "tmp") {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (!it->is_regular_file(entry_ec)) {
      continue;
    }
    if (name == "index.db" || name.rfind("index.db-", 0) == 0) {
      continue;
    }
    if (referenced.find(name) == referenced.end()) {
      std::error_code remove_ec;
      fs::remove(path, remove_ec);
    }
  }
}

void ImageCache::purgeMissingRows() {
  SQLite::Database &database = openDb(db_path_);
  struct Missing {
    std::string virtual_path;
    std::string variant;
  };
  std::vector<Missing> missing;
  {
    SQLite::Statement query(
        database,
        "SELECT virtual_path, variant, cache_file, bytes, is_local "
        "FROM image_cache");
    while (query.executeStep()) {
      const std::string virtual_path = query.getColumn(0).getString();
      const std::string variant = query.getColumn(1).getString();
      const std::string cache_file = query.getColumn(2).getString();
      const std::uintmax_t bytes =
          static_cast<std::uintmax_t>(query.getColumn(3).getInt64());
      const bool is_local = query.getColumn(4).getInt() != 0;
      const fs::path path =
          is_local ? fs::path(cache_file) : (opt_.dir / cache_file);
      std::error_code ec;
      const std::uintmax_t size = fs::file_size(path, ec);
      if (ec || size != bytes || (is_local && !underAnyRoot(path))) {
        missing.push_back({virtual_path, variant});
      }
    }
  }
  if (missing.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(db_write_mu_);
  for (const Missing &entry : missing) {
    SQLite::Statement remove(
        database,
        "DELETE FROM image_cache WHERE virtual_path=? AND variant=?");
    remove.bind(1, entry.virtual_path);
    remove.bind(2, entry.variant);
    remove.exec();
  }
}

void ImageCache::gcLoop() {
  while (!stopping_) {
    {
      std::unique_lock<std::mutex> lock(gc_mu_);
      gc_cv_.wait_for(lock,
                      std::chrono::seconds(std::max(1, opt_.gc_interval_seconds)),
                      [this]() { return stopping_.load(); });
    }
    if (stopping_) {
      break;
    }
    try {
      gc();
    } catch (const std::exception &ex) {
      std::cerr << "[image_cache] gc failed: " << ex.what() << '\n';
    }
  }
}

Stats ImageCache::stats() const {
  Stats result;
  try {
    SQLite::Database &database = openDb(db_path_);
    SQLite::Statement count(database, "SELECT COUNT(*) FROM image_cache");
    if (count.executeStep()) {
      result.entries =
          static_cast<std::uint64_t>(count.getColumn(0).getInt64());
    }
    SQLite::Statement bytes(
        database,
        "SELECT COALESCE(SUM(bytes), 0) FROM image_cache WHERE is_local=0");
    if (bytes.executeStep()) {
      result.bytes =
          static_cast<std::uintmax_t>(bytes.getColumn(0).getInt64());
    }
  } catch (const std::exception &) {
  }
  return result;
}

namespace {
std::mutex g_instance_mu;
std::shared_ptr<ImageCache> g_instance;
} // namespace

bool init(const Options &opt, std::string &error) {
  std::lock_guard<std::mutex> lock(g_instance_mu);
  if (g_instance) {
    return true;
  }
  auto instance = std::make_shared<ImageCache>(opt);
  if (!instance->init(error)) {
    return false;
  }
  g_instance = std::move(instance);
  return true;
}

void shutdown() {
  std::shared_ptr<ImageCache> instance;
  {
    std::lock_guard<std::mutex> lock(g_instance_mu);
    instance = std::move(g_instance);
  }
  if (instance) {
    instance->shutdown();
  }
}

std::shared_ptr<ImageCache> get() {
  std::lock_guard<std::mutex> lock(g_instance_mu);
  return g_instance;
}

} // namespace image_cache
