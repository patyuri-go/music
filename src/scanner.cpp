#include "scanner.h"

#include "config.h"
#include "database.h"
#include "db.h"
#include "metadata.h"
#include "schema.h"
#include "storage.h"

#include "api/webdav/webdav.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {

constexpr std::string_view kAudioExtensions[] = {
    ".mp3", ".flac", ".m4a", ".ogg", ".opus", ".wav", ".aac", ".wma"};

constexpr std::string_view kImageExtensions[] = {
    ".jpg", ".jpeg", ".png", ".webp"};

bool isAudioFile(const fs::path &p) {
  const std::string ext = p.extension().string();
  for (const auto &e : kAudioExtensions) {
    if (ext == e) {
      return true;
    }
  }
  return false;
}

bool isImageFile(const fs::path &p) {
  const std::string ext = p.extension().string();
  for (const auto &e : kImageExtensions) {
    if (ext == e) {
      return true;
    }
  }
  return false;
}

bool isCoverFile(const fs::path &p) {
  std::string stem = p.stem().string();
  for (char &c : stem) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return stem == "cover" || stem == "folder" || stem == "front" ||
         stem == "album" || stem == "artwork" || stem == "albumart";
}

std::string canonicalString(const fs::path &p) {
  std::error_code ec;
  const fs::path c = fs::canonical(p, ec);
  if (ec) {
    return p.string();
  }
  return c.string();
}

} // namespace

namespace scanner {
namespace {

std::mutex g_mutex;
std::atomic<bool> g_stop{false};
std::thread g_thread;
Progress g_progress;
ProgressCb g_cb;

// ---- Background job queue ------------------------------------------------
std::mutex g_job_mutex;
std::condition_variable g_job_cv;
std::deque<std::pair<int, std::function<void()>>> g_jobs;
std::map<int, JobStatus> g_job_status;
bool g_worker_started = false;
int g_next_job_id = 1;

// ---- Concurrency primitives for parallel scanning ------------------------
// Serialises all database writes (SQLite connections are thread_local, but
// concurrent writers must not race on the same rows / hit SQLITE_BUSY).
std::recursive_mutex g_db_mutex;

// Per-worker "currently scanning" path, surfaced through /api/jobs.
std::mutex g_active_mutex;
std::vector<std::string> g_active_paths;
int g_active_threads = 0;
// Current file of a non-pool job (e.g. sync-from-file / sync-to-file).
std::string g_current_file;

void beginActivePaths(int n) {
  std::lock_guard<std::mutex> lock(g_active_mutex);
  g_active_paths.assign(static_cast<std::size_t>(n < 1 ? 1 : n), std::string());
  g_active_threads = n < 1 ? 1 : n;
}

void endActivePaths() {
  std::lock_guard<std::mutex> lock(g_active_mutex);
  g_active_paths.clear();
  g_active_threads = 0;
}

void setActiveSlot(int slot, const std::string &path) {
  std::lock_guard<std::mutex> lock(g_active_mutex);
  if (slot >= 0 && static_cast<std::size_t>(slot) < g_active_paths.size()) {
    g_active_paths[slot] = path;
  }
}

void clearCurrentFile() {
  std::lock_guard<std::mutex> lock(g_active_mutex);
  g_current_file.clear();
}

void jobWorker() {
  while (true) {
    int id = 0;
    std::function<void()> work;
    {
      std::unique_lock<std::mutex> lock(g_job_mutex);
      g_job_cv.wait(lock, [] { return !g_jobs.empty(); });
      id = g_jobs.front().first;
      work = std::move(g_jobs.front().second);
      g_jobs.pop_front();
      g_job_status[id].state = "running";
    }
    try {
      work();
      std::lock_guard<std::mutex> lock(g_job_mutex);
      g_job_status[id].state = "done";
    } catch (const std::exception &e) {
      std::lock_guard<std::mutex> lock(g_job_mutex);
      g_job_status[id].state = "error";
      g_job_status[id].error = e.what();
    }
    clearCurrentFile();
    g_job_cv.notify_all();
  }
}

void report(const Progress &p) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_progress = p;
  if (g_cb) {
    g_cb(p);
  }
}

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

int ensureArtist(db::Session &db, const std::string &name) {
  std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
  db.execute(schema::sql::artistInsert(db.dialect()), {name});
  const auto result = db.query(schema::sql::kArtistByName, {name});
  if (!result.rows.empty()) {
    return static_cast<int>(intAt(result.rows.front(), 0));
  }
  return 0;
}

std::string artistKey(const std::vector<std::string> &names) {
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

std::vector<std::string> effectiveArtists(const metadata::TrackMeta &meta) {
  if (!meta.artists.empty()) {
    return meta.artists;
  }
  if (!meta.artist.empty()) {
    return {meta.artist};
  }
  return {};
}

std::vector<std::string> effectiveAlbumArtists(const metadata::TrackMeta &meta) {
  if (!meta.album_artists.empty()) {
    return meta.album_artists;
  }
  return effectiveArtists(meta);
}

int ensureAlbum(db::Session &db, const std::string &name,
                const std::string &key) {
  std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
  db.execute(schema::sql::albumUpsert(db.dialect()), {name, key});

  const auto result = db.query(schema::sql::kAlbumByKey, {name, key});
  if (!result.rows.empty()) {
    return static_cast<int>(intAt(result.rows.front(), 0));
  }
  return 0;
}

std::vector<int> ensureArtists(db::Session &db,
                               const std::vector<std::string> &names) {
  std::vector<int> ids;
  for (const auto &name : names) {
    if (name.empty()) {
      continue;
    }
    const int id = ensureArtist(db, name);
    if (id != 0) {
      ids.push_back(id);
    }
  }
  return ids;
}

void linkTrackArtists(db::Session &db, int track_id,
                      const std::vector<int> &artist_ids) {
  std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
  db.execute(schema::sql::kTrackArtistClear, {static_cast<std::int64_t>(track_id)});

  int position = 0;
  for (const int artist_id : artist_ids) {
    if (artist_id == 0) {
      continue;
    }
    db.execute(schema::sql::trackArtistInsert(db.dialect()),
               {static_cast<std::int64_t>(track_id),
                static_cast<std::int64_t>(artist_id),
                static_cast<std::int64_t>(position++)});
  }
}

void linkAlbumArtists(db::Session &db, int album_id,
                      const std::vector<int> &artist_ids) {
  std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
  db.execute(schema::sql::kAlbumArtistClear, {static_cast<std::int64_t>(album_id)});

  int position = 0;
  for (const int artist_id : artist_ids) {
    if (artist_id == 0) {
      continue;
    }
    db.execute(schema::sql::albumArtistInsert(db.dialect()),
               {static_cast<std::int64_t>(album_id),
                static_cast<std::int64_t>(artist_id),
                static_cast<std::int64_t>(position++)});
  }
}

int ensureGenre(db::Session &db, const std::string &name) {
  std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
  db.execute(schema::sql::genreInsert(db.dialect()), {name});

  const auto result = db.query(schema::sql::kGenreByName, {name});
  if (!result.rows.empty()) {
    return static_cast<int>(intAt(result.rows.front(), 0));
  }
  return 0;
}

void applyAlbumYear(db::Session &db, int album_id, int year) {
  if (album_id == 0 || year <= 0) {
    return;
  }
  std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
  db.execute(schema::sql::kAlbumUpdateYear,
             {static_cast<std::int64_t>(year), static_cast<std::int64_t>(album_id)});
}

void storeLyrics(db::Session &db, int track_id, const std::string &text) {
  if (track_id == 0 || text.empty()) {
    return;
  }
  std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
  db.execute(schema::sql::trackLyricsUpsert(db.dialect()),
             {static_cast<std::int64_t>(track_id), text,
              static_cast<std::int64_t>(metadata::isSyncedLyrics(text) ? 1 : 0)});
}

/// 歌词来源：优先文件内嵌歌词，其次同目录同名 .lrc 文件。
std::string fetchLyrics(const metadata::TrackMeta &meta,
                        const std::string &real_path) {
  if (!meta.lyrics.empty()) {
    return meta.lyrics;
  }
  return metadata::readSiblingLyrics(real_path);
}

int upsertTrack(db::Session &db, int root_id, const std::string &path,
                const metadata::TrackMeta &meta, int album_id,
                const std::vector<int> &artist_ids) {
  std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
  applyAlbumYear(db, album_id, meta.year);

  const auto check = db.query(schema::sql::kTrackByPath, {path});
  const bool exists = !check.rows.empty();

  if (exists) {
    const auto &row = check.rows.front();
    const int id = static_cast<int>(intAt(row, 0));
    const long long old_mtime = intAt(row, 1);
    const long long old_size = intAt(row, 2);

    if (old_mtime == static_cast<long long>(meta.mtime) &&
        old_size == static_cast<long long>(meta.size_bytes)) {
      return id;
    }

    db.execute(schema::sql::kTrackUpdate,
               {meta.title,
                static_cast<std::int64_t>(album_id),
                static_cast<std::int64_t>(meta.disc_no),
                static_cast<std::int64_t>(meta.track_no),
                static_cast<std::int64_t>(meta.duration_ms),
                static_cast<std::int64_t>(meta.bitrate),
                static_cast<std::int64_t>(meta.sample_rate),
                static_cast<std::int64_t>(meta.channels),
                meta.format,
                static_cast<std::int64_t>(meta.size_bytes),
                static_cast<std::int64_t>(meta.mtime),
                path});
    linkTrackArtists(db, id, artist_ids);
    return id;
  }

  db.execute(schema::sql::trackInsert(db.dialect()),
             {static_cast<std::int64_t>(root_id), path, meta.title,
              static_cast<std::int64_t>(album_id),
              static_cast<std::int64_t>(meta.disc_no),
              static_cast<std::int64_t>(meta.track_no),
              static_cast<std::int64_t>(meta.duration_ms),
              static_cast<std::int64_t>(meta.bitrate),
              static_cast<std::int64_t>(meta.sample_rate),
              static_cast<std::int64_t>(meta.channels), meta.format,
              static_cast<std::int64_t>(meta.size_bytes),
              static_cast<std::int64_t>(meta.mtime)});

  const int id = static_cast<int>(db.lastInsertId());
  linkTrackArtists(db, id, artist_ids);
  return id;
}

void linkGenre(db::Session &db, int track_id, const std::string &genre_name) {
  if (genre_name.empty()) {
    return;
  }
  std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
  const int genre_id = ensureGenre(db, genre_name);
  if (genre_id == 0) {
    return;
  }
  db.execute(schema::sql::trackGenreInsert(db.dialect()),
             {static_cast<std::int64_t>(track_id),
              static_cast<std::int64_t>(genre_id)});
}

long long parseHttpDate(const std::string &s) {
  std::tm tm{};
  if (strptime(s.c_str(), "%a, %d %b %Y %H:%M:%S", &tm) == nullptr) {
    return 0;
  }
  return static_cast<long long>(timegm(&tm));
}

// ---- Parallel directory traversal ----------------------------------------
void scanLocalFile(int root_id, const fs::path &file);
void scanWebdavFile(int root_id, const std::string &url);

struct DirTask {
  std::string dir;   // real local path or WebDAV URL (path for dirs, URL for files)
  std::string cover; // effective cover inherited from ancestors (virtual)
  bool is_file = false; // when true, `dir` is a single file to index
  long long size = 0;   // WebDAV file size (from PROPFIND)
  long long mtime = 0;  // WebDAV file mtime (epoch seconds, from PROPFIND)
};

void scanLocalTree(const repo::ScanRoot &sr, const std::vector<DirTask> &starts,
                   std::optional<std::reference_wrapper<Progress>> progress,
                   bool only_new = false);

struct ScanCtx {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<DirTask> q;
  std::set<std::string> visited;
  int active = 0;
  bool only_new = false; // skip files already indexed (do not read them)
};

void enqueueDir(ScanCtx &ctx, const DirTask &task,
                const std::string &visit_key) {
  if (g_stop.load(std::memory_order_relaxed)) {
    return;
  }
  std::lock_guard<std::mutex> lock(ctx.mu);
  if (!ctx.visited.insert(visit_key).second) {
    return;
  }
  ctx.q.push_back(task);
  ++ctx.active;
  ctx.cv.notify_one();
}

using DirProcessor = std::function<void(ScanCtx &, const DirTask &, int)>;

void runDirPool(ScanCtx &ctx, int threads, const DirProcessor &process) {
  if (threads < 1) {
    threads = 1;
  }
  std::vector<std::thread> pool;
  pool.reserve(static_cast<std::size_t>(threads));
  for (int i = 0; i < threads; ++i) {
    pool.emplace_back([&ctx, &process, i]() {
      while (true) {
        DirTask task;
        bool have = false;
        {
          std::unique_lock<std::mutex> lock(ctx.mu);
          ctx.cv.wait(lock, [&ctx]() {
            return g_stop.load(std::memory_order_relaxed) || !ctx.q.empty() ||
                   ctx.active == 0;
          });
          if (!g_stop.load(std::memory_order_relaxed) && !ctx.q.empty()) {
            task = std::move(ctx.q.front());
            ctx.q.pop_front();
            have = true;
          }
        }
        if (!have) {
          setActiveSlot(i, std::string());
          return;
        }
        setActiveSlot(i, task.dir);
        process(ctx, task, i);
        setActiveSlot(i, std::string());
        {
          std::lock_guard<std::mutex> lock(ctx.mu);
          if (--ctx.active == 0) {
            ctx.cv.notify_all();
          }
        }
      }
    });
  }
  for (auto &t : pool) {
    t.join();
  }
}

void scanWebdavTree(const webdav::Url &base, const std::vector<DirTask> &starts,
                    int root_id,
                    std::optional<std::reference_wrapper<Progress>> progress,
                    bool only_new = false) {
  webdav::Client client(base);
  repo::ScanRootRepo root_repo(db::mainSession());
  auto root_opt = root_repo.byId(root_id);
  const std::optional<repo::ScanRoot> &root = root_opt;

  ScanRootProgress local_rp;
  ScanRootProgress &rp =
      (progress.has_value() && root_id >= 1 &&
       static_cast<std::size_t>(root_id) <= progress->get().roots.size())
          ? progress->get().roots[root_id - 1]
          : local_rp;

  ScanCtx ctx;
  ctx.only_new = only_new;
  const int threads = std::max(1, config::get().local.scan_threads);
  beginActivePaths(threads);

  DirProcessor process = [&](ScanCtx &c, const DirTask &task, int slot) {
    if (g_stop.load(std::memory_order_relaxed)) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(c.mu);
      rp.current_file = task.dir;
    }

    if (task.is_file) {
      const std::string url = task.dir;
      const std::string vpath = root ? storage::toVirtual(*root, url) : url;
      if (c.only_new) {
        std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
        db::Session &db = db::mainSession();
        const auto check = db.query(schema::sql::kTrackByPath, {vpath});
        if (!check.rows.empty()) {
          std::lock_guard<std::mutex> lock(c.mu);
          rp.files_processed++;
          return;
        }
      }
      auto meta_opt = metadata::read(url);
      if (!meta_opt) {
        std::lock_guard<std::mutex> lock(c.mu);
        rp.errors++;
        return;
      }
      metadata::TrackMeta meta = *meta_opt;
      meta.size_bytes = task.size;
      meta.mtime = task.mtime;
      const std::string lyrics = fetchLyrics(meta, url);

      int added = 0;
      int updated = 0;
      {
        std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
        db::Session &db = db::mainSession();
        const auto check = db.query(schema::sql::kTrackByPath, {vpath});
        const bool exists = !check.rows.empty();
        if (exists) {
          const auto &row = check.rows.front();
          const long long old_mtime = intAt(row, 1);
          const long long old_size = intAt(row, 2);
          if (old_mtime == static_cast<long long>(meta.mtime) &&
              old_size == static_cast<long long>(meta.size_bytes)) {
            applyAlbumYear(db, static_cast<int>(intAt(row, 3)), meta.year);
            std::lock_guard<std::mutex> lock(c.mu);
            rp.files_processed++;
            return;
          }
        }

        const std::vector<std::string> artist_names = effectiveArtists(meta);
        const std::vector<int> artist_ids = ensureArtists(db, artist_names);
        const std::vector<std::string> album_artist_names =
            effectiveAlbumArtists(meta);
        const int album_id =
            ensureAlbum(db, meta.album, artistKey(album_artist_names));
        const int track_id =
            upsertTrack(db, root_id, vpath, meta, album_id, artist_ids);
        if (track_id != 0) {
          storeLyrics(db, track_id, lyrics);
        }
        if (album_id != 0) {
          linkAlbumArtists(db, album_id, ensureArtists(db, album_artist_names));
          if (!task.cover.empty()) {
            db.execute(schema::sql::kAlbumUpdateCover,
                       {task.cover, static_cast<std::int64_t>(album_id)});
          }
        }
        if (exists) {
          updated = 1;
        } else {
          if (track_id != 0) {
            linkGenre(db, track_id, meta.genre);
          }
          added = 1;
        }
      }
      {
        std::lock_guard<std::mutex> lock(c.mu);
        rp.files_processed++;
        rp.files_added += added;
        rp.files_updated += updated;
      }
      return;
    }

    webdav::Response res = client.propfind(task.dir, 1);
    if (res.status != 207) {
      std::lock_guard<std::mutex> lock(c.mu);
      rp.errors++;
      return;
    }
    const std::vector<webdav::Resource> items =
        webdav::parsePropfind(res.body);

    std::string dir_cover = task.cover;
    for (const auto &item : items) {
      std::string path = item.path;
      if (path.size() > 1 && path.back() == '/') {
        path.pop_back();
      }
      if (path == task.dir || item.collection) {
        continue;
      }
      const std::size_t slash = path.rfind('/');
      const std::string name =
          (slash == std::string::npos) ? path : path.substr(slash + 1);
      const fs::path name_path(name);
      if (isCoverFile(name_path) && isImageFile(name_path)) {
        const std::string cover_url = webdav::buildUrl(base, path);
        dir_cover = root ? storage::toVirtual(*root, cover_url) : cover_url;
        break;
      }
    }

    for (const auto &item : items) {
      std::string path = item.path;
      if (path.size() > 1 && path.back() == '/') {
        path.pop_back();
      }
      if (path == task.dir) {
        continue;
      }
      const std::size_t slash = path.rfind('/');
      const std::string name =
          (slash == std::string::npos) ? path : path.substr(slash + 1);
      if (name.empty()) {
        continue;
      }
      if (item.collection) {
        enqueueDir(c, DirTask{path, dir_cover, false, 0, 0}, path);
        continue;
      }
      const fs::path name_path(name);
      if (!isAudioFile(name_path)) {
        continue;
      }
      const std::string url = webdav::buildUrl(base, path);
      const std::string vpath = root ? storage::toVirtual(*root, url) : url;
      enqueueDir(c,
                 DirTask{url, dir_cover, true, item.size,
                         parseHttpDate(item.last_modified)},
                 vpath);
    }
  };

  ctx.active = 0;
  for (const auto &s : starts) {
    const std::string key = s.dir.empty() ? "/" : s.dir;
    if (ctx.visited.insert(key).second) {
      ctx.q.push_back(s);
      ++ctx.active;
    }
  }
  if (ctx.active > 0) {
    runDirPool(ctx, threads, process);
  }
  endActivePaths();

  if (!g_stop.load(std::memory_order_relaxed)) {
    std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
    db::Session &db = db::mainSession();
    db.execute(schema::sql::scanRootUpdateLastScan(db.dialect()),
               {static_cast<std::int64_t>(root_id)});
  }
}

void scanWebdavRoot(const repo::ScanRoot &sr, Progress &progress) {
  auto &rp = progress.roots[sr.id - 1];
  rp.scanning = true;
  try {
    const webdav::Url base = webdav::resolveCredentials(sr.path);
    const std::string start = base.path.empty() ? "/" : base.path;
    scanWebdavTree(base, {DirTask{start, "", false}}, sr.id, std::ref(progress));
  } catch (const std::exception &e) {
    std::cerr << "[scanner] scanWebdavRoot exception: " << e.what()
              << std::endl;
    rp.errors++;
  }
  rp.scanning = false;
}

void scanWebdavDir(int root_id, const std::string &url) {
  try {
    const webdav::Url base = webdav::resolveCredentials(url);
    webdav::Url target;
    if (!webdav::parseUrl(url, target)) {
      return;
    }
    scanWebdavTree(base, {DirTask{target.path, "", false}}, root_id, std::nullopt);
  } catch (const std::exception &e) {
    std::cerr << "[scanner] scanWebdavDir exception: " << e.what() << std::endl;
  }
}

void scanRoot(const repo::ScanRoot &sr, Progress &progress) {
  if (webdav::isWebdavUrl(sr.path)) {
    scanWebdavRoot(sr, progress);
    return;
  }
  scanLocalTree(sr, {}, std::ref(progress));
}

void scanLocalTree(const repo::ScanRoot &sr, const std::vector<DirTask> &starts,
                   std::optional<std::reference_wrapper<Progress>> progress,
                   bool only_new) {
  ScanRootProgress local_rp;
  ScanRootProgress &rp =
      (progress.has_value() && sr.id >= 1 &&
       static_cast<std::size_t>(sr.id) <= progress->get().roots.size())
          ? progress->get().roots[sr.id - 1]
          : local_rp;
  rp.scanning = true;
  try {
    const auto options =
        config::get().local.follow_symlinks
            ? fs::directory_options::follow_directory_symlink
            : fs::directory_options::none;

    ScanCtx ctx;
    ctx.only_new = only_new;
    const int threads = std::max(1, config::get().local.scan_threads);
    beginActivePaths(threads);

    DirProcessor process = [&](ScanCtx &c, const DirTask &task, int slot) {
      if (g_stop.load(std::memory_order_relaxed)) {
        return;
      }
      const fs::path current(task.dir);
      {
        std::lock_guard<std::mutex> lock(c.mu);
        rp.current_file = task.dir;
      }

      if (task.is_file) {
        std::error_code ec2;
        const fs::path canonical_path = fs::canonical(current, ec2);
        const std::string path_str =
            ec2 ? current.string() : canonical_path.string();
        const std::string vpath = storage::toVirtual(sr, path_str);

        if (c.only_new) {
          std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
          db::Session &db = db::mainSession();
          const auto check = db.query(schema::sql::kTrackByPath, {vpath});
          if (!check.rows.empty()) {
            std::lock_guard<std::mutex> lock(c.mu);
            rp.files_processed++;
            return;
          }
        }

        auto meta_opt = metadata::read(path_str);
        if (!meta_opt) {
          std::lock_guard<std::mutex> lock(c.mu);
          rp.errors++;
          return;
        }
        const metadata::TrackMeta &meta = *meta_opt;
        const std::string lyrics = fetchLyrics(meta, path_str);

        int added = 0;
        int updated = 0;
        {
          std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
          db::Session &db = db::mainSession();
          const auto check = db.query(schema::sql::kTrackByPath, {vpath});
          const bool exists = !check.rows.empty();
          if (exists) {
            const auto &row = check.rows.front();
            const long long old_mtime = intAt(row, 1);
            const long long old_size = intAt(row, 2);
            if (old_mtime == static_cast<long long>(meta.mtime) &&
                old_size == static_cast<long long>(meta.size_bytes)) {
              applyAlbumYear(db, static_cast<int>(intAt(row, 3)), meta.year);
              std::lock_guard<std::mutex> lock(c.mu);
              rp.files_processed++;
              return;
            }
          }

          const std::vector<std::string> artist_names = effectiveArtists(meta);
          const std::vector<int> artist_ids = ensureArtists(db, artist_names);
          const std::vector<std::string> album_artist_names =
              effectiveAlbumArtists(meta);
          const int album_id =
              ensureAlbum(db, meta.album, artistKey(album_artist_names));
          const int track_id =
              upsertTrack(db, sr.id, vpath, meta, album_id, artist_ids);
          if (track_id != 0) {
            storeLyrics(db, track_id, lyrics);
          }
          if (album_id != 0) {
            linkAlbumArtists(db, album_id,
                             ensureArtists(db, album_artist_names));
            if (!task.cover.empty()) {
              db.execute(schema::sql::kAlbumUpdateCover,
                         {task.cover, static_cast<std::int64_t>(album_id)});
            }
          }
          if (exists) {
            updated = 1;
          } else {
            if (track_id != 0) {
              linkGenre(db, track_id, meta.genre);
            }
            added = 1;
          }
        }
        {
          std::lock_guard<std::mutex> lock(c.mu);
          rp.files_processed++;
          rp.files_added += added;
          rp.files_updated += updated;
        }
        return;
      }

      std::vector<fs::path> entries;
      {
        std::error_code ec_dir;
        for (const auto &entry :
             fs::directory_iterator(current, options, ec_dir)) {
          if (ec_dir) {
            break;
          }
          entries.push_back(entry.path());
        }
      }

      std::string dir_cover = task.cover;
      for (const auto &p : entries) {
        if (isCoverFile(p) && isImageFile(p)) {
          std::error_code ec2;
          const fs::path cp = fs::canonical(p, ec2);
          dir_cover = storage::toVirtual(sr, ec2 ? p.string() : cp.string());
          break;
        }
      }

      for (const auto &p : entries) {
        std::error_code ec_type;
        const bool is_dir = fs::is_directory(p, ec_type);
        if (ec_type) {
          continue;
        }
        if (is_dir) {
          enqueueDir(c, DirTask{p.string(), dir_cover, false, 0, 0},
                     canonicalString(p));
          continue;
        }
        if (!isAudioFile(p)) {
          continue;
        }
        std::error_code ec2;
        const fs::path canonical_path = fs::canonical(p, ec2);
        const std::string path_str =
            ec2 ? p.string() : canonical_path.string();
        enqueueDir(c, DirTask{path_str, dir_cover, true, 0, 0}, path_str);
      }
    };

    if (starts.empty()) {
      std::error_code ec;
      const fs::path root_path = fs::weakly_canonical(sr.path, ec);
      if (ec || !fs::is_directory(root_path, ec)) {
        endActivePaths();
        rp.errors++;
        rp.scanning = false;
        return;
      }
      ctx.visited.insert(canonicalString(root_path));
      ctx.q.push_back(DirTask{root_path.string(), "", false});
      ctx.active = 1;
    } else {
      ctx.active = 0;
      for (const auto &s : starts) {
        const std::string key = canonicalString(fs::path(s.dir));
        if (ctx.visited.insert(key).second) {
          ctx.q.push_back(s);
          ++ctx.active;
        }
      }
    }
    if (ctx.active > 0) {
      runDirPool(ctx, threads, process);
    }
    endActivePaths();

    if (!g_stop.load(std::memory_order_relaxed)) {
      std::lock_guard<std::recursive_mutex> dblock(g_db_mutex);
      db::Session &db = db::mainSession();
      db.execute(schema::sql::scanRootUpdateLastScan(db.dialect()),
                 {static_cast<std::int64_t>(sr.id)});
    }

    rp.scanning = false;
  } catch (const std::exception &e) {
    std::cerr << "[scanner] scanLocalTree exception: " << e.what() << std::endl;
    rp.errors++;
    rp.scanning = false;
  }
}

void runScanAll() {
  try {
    repo::ScanRootRepo repo(db::mainSession());
    auto roots = repo.list();

    Progress progress;
    progress.scanning = true;
    progress.total_roots = static_cast<int>(roots.size());
    progress.roots.resize(roots.size());

    for (size_t i = 0; i < roots.size(); ++i) {
      if (g_stop.load(std::memory_order_relaxed)) {
        break;
      }
      progress.current_root_index = static_cast<int>(i);
      progress.roots[i].id = roots[i].id;
      progress.roots[i].path = roots[i].path;
      scanRoot(roots[i], progress);
      report(progress);
    }

    progress.scanning = false;
    report(progress);
  } catch (const std::exception &e) {
    std::cerr << "[scanner] exception: " << e.what() << std::endl;
  }
}

void scanThread() { runScanAll(); }

void scanLocalFile(int root_id, const fs::path &file) {
  try {
    std::error_code ec2;
    const fs::path canonical_path = fs::canonical(file, ec2);
    const std::string path_str = ec2 ? file.string() : canonical_path.string();

    db::Session &db = db::mainSession();
    repo::ScanRootRepo root_repo(db::mainSession());
    auto root_opt = root_repo.byId(root_id);
    const std::optional<repo::ScanRoot> &root = root_opt;
    const std::string vpath =
        root ? storage::toVirtual(*root, path_str) : path_str;
    const auto check = db.query(schema::sql::kTrackByPath, {vpath});
    const bool exists = !check.rows.empty();

    auto meta_opt = metadata::read(path_str);
    if (!meta_opt) {
      return;
    }
    const metadata::TrackMeta &meta = *meta_opt;

    if (exists) {
      applyAlbumYear(db, static_cast<int>(intAt(check.rows.front(), 3)), meta.year);
      return;
    }

    const std::vector<std::string> artist_names = effectiveArtists(meta);
    const std::vector<int> artist_ids = ensureArtists(db, artist_names);
    const std::vector<std::string> album_artist_names =
        effectiveAlbumArtists(meta);
    const int album_id =
        ensureAlbum(db, meta.album, artistKey(album_artist_names));

    const int track_id =
        upsertTrack(db, root_id, vpath, meta, album_id, artist_ids);
    if (track_id != 0) {
      linkGenre(db, track_id, meta.genre);
      linkAlbumArtists(db, album_id, ensureArtists(db, album_artist_names));
      storeLyrics(db, track_id, fetchLyrics(meta, path_str));
    }

    db.execute(schema::sql::scanRootUpdateLastScan(db.dialect()),
               {static_cast<std::int64_t>(root_id)});
  } catch (const std::exception &e) {
    std::cerr << "[scanner] scanLocalFile exception: " << e.what() << std::endl;
  }
}

void scanWebdavFile(int root_id, const std::string &url) {
  try {
    const webdav::Url base = webdav::resolveCredentials(url);
    webdav::Client client(base);
    webdav::Url target;
    if (!webdav::parseUrl(url, target)) {
      return;
    }

    db::Session &db = db::mainSession();
    repo::ScanRootRepo root_repo(db::mainSession());
    auto root_opt = root_repo.byId(root_id);
    const std::optional<repo::ScanRoot> &root = root_opt;
    const std::string vpath = root ? storage::toVirtual(*root, url) : url;
    const auto check = db.query(schema::sql::kTrackByPath, {vpath});
    const bool exists = !check.rows.empty();

    auto meta_opt = metadata::read(url);
    if (!meta_opt) {
      return;
    }
    metadata::TrackMeta meta = *meta_opt;
    meta.size_bytes = client.contentLength(target.path);
    meta.mtime =
        parseHttpDate(client.head(target.path).header("last-modified"));

    if (exists) {
      applyAlbumYear(db, static_cast<int>(intAt(check.rows.front(), 3)),
                     meta.year);
      return;
    }

    const std::vector<std::string> artist_names = effectiveArtists(meta);
    const std::vector<int> artist_ids = ensureArtists(db, artist_names);
    const std::vector<std::string> album_artist_names =
        effectiveAlbumArtists(meta);
    const int album_id =
        ensureAlbum(db, meta.album, artistKey(album_artist_names));

    const int track_id =
        upsertTrack(db, root_id, vpath, meta, album_id, artist_ids);
    if (track_id != 0) {
      linkGenre(db, track_id, meta.genre);
      linkAlbumArtists(db, album_id, ensureArtists(db, album_artist_names));
      storeLyrics(db, track_id, fetchLyrics(meta, url));
    }

    db.execute(schema::sql::scanRootUpdateLastScan(db.dialect()),
               {static_cast<std::int64_t>(root_id)});
  } catch (const std::exception &e) {
    std::cerr << "[scanner] scanWebdavFile exception: " << e.what()
              << std::endl;
  }
}

} // namespace (anonymous inside scanner)

void scanDir(int root_id, const std::string &dir_path) {
  if (webdav::isWebdavUrl(dir_path)) {
    webdav::Url target;
    if (!webdav::parseUrl(dir_path, target)) {
      return;
    }
    const webdav::Url base = webdav::resolveCredentials(dir_path);
    webdav::Client client(base);
    webdav::Response res = client.propfind(target.path, 0);
    bool is_collection = false;
    if (res.status == 207) {
      const std::vector<webdav::Resource> items =
          webdav::parsePropfind(res.body);
      if (!items.empty()) {
        is_collection = items.front().collection;
      }
    }
    if (is_collection) {
      scanWebdavDir(root_id, dir_path);
    } else {
      scanWebdavFile(root_id, dir_path);
    }
    return;
  }
  try {
    std::error_code ec;
    const fs::path dir = fs::weakly_canonical(dir_path, ec);
    if (ec) {
      return;
    }
    if (fs::is_regular_file(dir, ec)) {
      scanLocalFile(root_id, dir);
      return;
    }
    if (!fs::is_directory(dir, ec)) {
      return;
    }

    repo::ScanRootRepo root_repo(db::mainSession());
    auto root_opt = root_repo.byId(root_id);
    if (!root_opt) {
      return;
    }
    scanLocalTree(*root_opt, {DirTask{dir.string(), "", false, 0, 0}}, std::nullopt);
  } catch (const std::exception &e) {
    std::cerr << "[scanner] scanDir exception: " << e.what() << std::endl;
  }
}

void scanDirBatch(int root_id, const std::vector<std::string> &paths) {
  if (paths.empty()) {
    return;
  }
  try {
    repo::ScanRootRepo root_repo(db::mainSession());
    auto root_opt = root_repo.byId(root_id);
    if (!root_opt) {
      return;
    }
    const repo::ScanRoot &root = *root_opt;

    if (webdav::isWebdavUrl(root.path)) {
      const webdav::Url base = webdav::resolveCredentials(root.path);
      std::vector<DirTask> starts;
      for (const auto &p : paths) {
        webdav::Url target;
        if (!webdav::parseUrl(p, target)) {
          continue;
        }
        webdav::Client client(base);
        webdav::Response res = client.propfind(target.path, 0);
        bool is_collection = false;
        long long size = 0;
        long long mtime = 0;
        if (res.status == 207) {
          const std::vector<webdav::Resource> items =
              webdav::parsePropfind(res.body);
          if (!items.empty()) {
            is_collection = items.front().collection;
            size = items.front().size;
            mtime = parseHttpDate(items.front().last_modified);
          }
        }
        if (is_collection) {
          starts.push_back(DirTask{target.path, "", false, 0, 0});
        } else {
          starts.push_back(DirTask{p, "", true, size, mtime});
        }
      }
      scanWebdavTree(base, starts, root_id, std::nullopt, true);
      return;
    }

    std::vector<DirTask> starts;
    for (const auto &p : paths) {
      std::error_code ec;
      const fs::path canonical_path = fs::weakly_canonical(p, ec);
      if (ec) {
        continue;
      }
      if (fs::is_regular_file(canonical_path, ec)) {
        starts.push_back(DirTask{canonical_path.string(), "", true});
      } else if (fs::is_directory(canonical_path, ec)) {
        starts.push_back(DirTask{canonical_path.string(), "", false});
      }
    }
    scanLocalTree(root, starts, std::nullopt, true);
  } catch (const std::exception &e) {
    std::cerr << "[scanner] scanDirBatch exception: " << e.what() << std::endl;
  }
}

void startScan(ProgressCb cb) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_progress.scanning) {
    return;
  }
  g_stop.store(false, std::memory_order_relaxed);
  g_cb = std::move(cb);
  g_progress = Progress{};
  g_progress.scanning = true;
  g_thread = std::thread(scanThread);
  g_thread.detach();
}

void stopScan() {
  g_stop.store(true, std::memory_order_relaxed);
}

Progress status() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_progress;
}

void scanAllBlocking() { runScanAll(); }

int enqueueJob(const std::string &kind, std::function<void()> work) {
  std::lock_guard<std::mutex> lock(g_job_mutex);
  if (!g_worker_started) {
    g_worker_started = true;
    std::thread(jobWorker).detach();
  }
  const int id = g_next_job_id++;
  JobStatus st;
  st.id = id;
  st.kind = kind;
  st.state = "queued";
  g_job_status[id] = st;
  g_jobs.emplace_back(id, std::move(work));
  g_job_cv.notify_one();
  return id;
}

namespace {
// Surface the live per-root scan counters on the currently running job so the
// jobs UI can show progress. Only meaningful while a full scan is reporting.
void mergeScanProgress(JobStatus &j) {
  if (j.state != "running") {
    return;
  }
  Progress p;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    p = g_progress;
  }
  if (!p.scanning) {
    return;
  }
  for (const auto &r : p.roots) {
    j.processed += r.files_processed;
    j.added += r.files_added;
    j.updated += r.files_updated;
    j.errors += r.errors;
  }
}
} // namespace

JobStatus jobStatus(int id) {
  JobStatus out;
  {
    std::lock_guard<std::mutex> lock(g_job_mutex);
    const auto it = g_job_status.find(id);
    if (it == g_job_status.end()) {
      return JobStatus{};
    }
    out = it->second;
  }
  if (out.state == "running") {
    std::lock_guard<std::mutex> lock(g_active_mutex);
    out.threads = g_active_threads;
    for (const auto &p : g_active_paths) {
      if (!p.empty()) {
        out.current_paths.push_back(p);
      }
    }
    if (out.current_paths.empty() && !g_current_file.empty()) {
      out.current_paths.push_back(g_current_file);
    }
    if (!out.current_paths.empty()) {
      out.current = out.current_paths.front();
    }
  }
  mergeScanProgress(out);
  return out;
}
std::vector<JobStatus> jobList() {
  std::vector<JobStatus> out;
  {
    std::lock_guard<std::mutex> lock(g_job_mutex);
    out.reserve(g_job_status.size());
    for (const auto &kv : g_job_status) {
      out.push_back(kv.second);
    }
  }
  std::vector<std::string> active;
  int threads = 0;
  std::string current_file;
  {
    std::lock_guard<std::mutex> lock(g_active_mutex);
    active = g_active_paths;
    threads = g_active_threads;
    current_file = g_current_file;
  }
  for (auto &j : out) {
    if (j.state != "running") {
      continue;
    }
    j.threads = threads;
    for (const auto &p : active) {
      if (!p.empty()) {
        j.current_paths.push_back(p);
      }
    }
    if (j.current_paths.empty() && !current_file.empty()) {
      j.current_paths.push_back(current_file);
    }
    if (!j.current_paths.empty()) {
      j.current = j.current_paths.front();
    }
  }
  for (auto &j : out) {
    mergeScanProgress(j);
  }
  return out;
}

void setCurrentFile(const std::string &path) {
  std::lock_guard<std::mutex> lock(g_active_mutex);
  g_current_file = path;
}

void activePathsBegin(int n) { beginActivePaths(n); }

void activePathsEnd() { endActivePaths(); }

void activePathsSet(int slot, const std::string &path) {
  setActiveSlot(slot, path);
}

} // namespace scanner
