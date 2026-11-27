#include "storage_client.h"

#include "http_util.h"

#include <curl/curl.h>
#include <taglib/aifffile.h>
#include <taglib/apefile.h>
#include <taglib/dsdifffile.h>
#include <taglib/dsffile.h>
#include <taglib/flacfile.h>
#include <taglib/mp4file.h>
#include <taglib/mpcfile.h>
#include <taglib/mpegfile.h>
#include <taglib/oggflacfile.h>
#include <taglib/opusfile.h>
#include <taglib/trueaudiofile.h>
#include <taglib/vorbisfile.h>
#include <taglib/wavfile.h>
#include <taglib/wavpackfile.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace storage_client {
namespace {

constexpr std::size_t kHeadBytes = 64 * 1024;
constexpr std::size_t kMaxProbeBytes = 1024 * 1024;

// ------------------------------------------------------------------ 线程池

class WorkerPool {
public:
  explicit WorkerPool(int count) {
    for (int i = 0; i < count; ++i) {
      workers_.emplace_back([this] { loop(); });
    }
  }

  ~WorkerPool() { stop(); }

  WorkerPool(const WorkerPool &) = delete;
  WorkerPool &operator=(const WorkerPool &) = delete;

  void submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        return;
      }
      tasks_.push(std::move(task));
    }
    cv_.notify_one();
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (std::thread &worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

private:
  void loop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty()) {
          if (stopping_) {
            return;
          }
          continue;
        }
        task = std::move(tasks_.front());
        tasks_.pop();
      }
      task();
    }
  }

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stopping_ = false;
};

// ------------------------------------------------------------------ 全局

Options g_options;
std::mutex g_init_mutex;
std::unique_ptr<WorkerPool> g_pool;
std::atomic<bool> g_ready{false};
std::once_flag g_curl_once;

std::string trim(std::string text) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  text.erase(text.begin(),
             std::find_if(text.begin(), text.end(), not_space));
  text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(),
             text.end());
  return text;
}



std::string api_url(const std::string &entity_id) {
  return g_options.scheme + "://" + g_options.host + ":" +
         std::to_string(g_options.port) + "/api/entities/" + entity_id +
         "/content";
}

// ------------------------------------------------------------------ HTTP

} // namespace

// ------------------------------------------------------------------ 对外

void initialize(const Options &options) {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  if (g_ready.load()) {
    return;
  }
  g_options = options;
  std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  g_pool = std::make_unique<WorkerPool>(std::max(1, options.workers));
  g_ready.store(true);
}

void shutdown() {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  if (!g_ready.exchange(false)) {
    return;
  }
  g_pool.reset();
}

bool enabled() { return g_ready.load(); }

std::string content_url(const std::string &entity_id) {
  return api_url(trim(entity_id));
}

int timeout_ms() { return g_options.timeout_ms; }

bool valid_entity_id(const std::string &entity_id) {
  const std::string id = trim(entity_id);
  if (id.size() != 36) {
    return false;
  }
  for (std::size_t i = 0; i < id.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (id[i] != '-') {
        return false;
      }
    } else if (!std::isxdigit(static_cast<unsigned char>(id[i]))) {
      return false;
    }
  }
  return true;
}


} // namespace storage_client