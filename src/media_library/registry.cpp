#include "media_library/registry.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace media_library {

std::string_view platform_kind_name(PlatformKind kind) noexcept {
  switch (kind) {
  case PlatformKind::Local:
    return "local";
  case PlatformKind::WebDav:
    return "webdav";
  case PlatformKind::NetEase:
    return "netease";
  case PlatformKind::Other:
    return "other";
  }
  return "other";
}

PlatformKind platform_kind_from(std::string_view text) noexcept {
  if (text == "local") {
    return PlatformKind::Local;
  }
  if (text == "webdav") {
    return PlatformKind::WebDav;
  }
  if (text == "netease" || text == "cloudmusic") {
    return PlatformKind::NetEase;
  }
  return PlatformKind::Other;
}

std::string_view health_state_name(HealthState state) noexcept {
  switch (state) {
  case HealthState::Ok:
    return "ok";
  case HealthState::Degraded:
    return "degraded";
  case HealthState::Unauthenticated:
    return "unauthenticated";
  case HealthState::RateLimited:
    return "rate_limited";
  case HealthState::Unavailable:
    return "unavailable";
  }
  return "unavailable";
}

namespace {

// 健康状态排序权重：数字越小越优先（5.2.5 排序信号第 3 项）。
int health_rank(HealthState state) noexcept {
  switch (state) {
  case HealthState::Ok:
    return 0;
  case HealthState::Degraded:
    return 1;
  case HealthState::RateLimited:
    return 2;
  case HealthState::Unauthenticated:
    return 3;
  case HealthState::Unavailable:
    return 4;
  }
  return 4;
}

bool is_open_circuit_failure(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::ProviderUnavailable:
  case ErrorCode::Timeout:
  case ErrorCode::Internal:
    return true;
  case ErrorCode::InvalidRequest:
  case ErrorCode::NotFound:
  case ErrorCode::AuthRequired:
  case ErrorCode::PermissionDenied:
  case ErrorCode::Unsupported:
  case ErrorCode::RateLimited:
  case ErrorCode::PlaybackExpired:
  case ErrorCode::Conflict:
  case ErrorCode::Cancelled:
    return false;
  }
  return false;
}

struct Entry {
  std::shared_ptr<Library> library;
  LibraryRegistration registration;
  CapabilitySet capabilities;
  HealthSnapshot health;

  // 运行时状态。
  int in_flight = 0;
  int consecutive_failures = 0;
  std::int64_t circuit_open_until_ms = 0;
  std::int64_t rate_limited_until_ms = 0;
  bool stopping = false;
  int registration_order = 0;
};

class LibraryRegistryImpl final : public LibraryRegistry {
public:
  Status add(std::shared_ptr<Library> library,
             const LibraryRegistration &registration) override {
    if (!library) {
      return Status::failure(
          Error::make(ErrorCode::InvalidRequest, "library must not be null"));
    }
    if (registration.library_id.empty()) {
      return Status::failure(
          Error::make(ErrorCode::InvalidRequest, "library_id must not be empty"));
    }

    const LibraryIdentity identity = library->identity();
    if (identity.library_id != registration.library_id) {
      return Status::failure(Error::make(
          ErrorCode::InvalidRequest,
          "registration.library_id does not match library identity"));
    }

    CapabilitySet capabilities = library->capabilities();

    std::unique_lock lock(mutex_);
    if (stopping_) {
      return Status::failure(Error::make(
          ErrorCode::ProviderUnavailable, "registry is shutting down"));
    }
    if (entries_.count(registration.library_id) != 0) {
      return Status::failure(Error::make(
          ErrorCode::Conflict,
          "library already registered: " + registration.library_id));
    }

    Entry entry;
    entry.library = std::move(library);
    entry.registration = registration;
    entry.capabilities = std::move(capabilities);
    entry.health.state = HealthState::Ok;
    entry.health.checked_at = 0;
    entry.registration_order = next_order_++;

    entries_.emplace(registration.library_id, std::move(entry));
    rebuild_index_locked();
    return Status::success();
  }

  Status remove(std::string_view library_id) override {
    std::shared_ptr<Library> closed;
    {
      std::unique_lock lock(mutex_);
      const auto it = entries_.find(std::string(library_id));
      if (it == entries_.end()) {
        return Status::failure(Error::make(
            ErrorCode::NotFound,
            "library not registered: " + std::string(library_id)));
      }
      it->second.stopping = true;
      closed = it->second.library;
      entries_.erase(it);
      rebuild_index_locked();
    }
    // close() 在锁外调用：它可能阻塞，不能让注册表跟着卡住。
    if (closed) {
      closed->close();
    }
    return Status::success();
  }

  std::optional<LibraryRegistration>
  find(std::string_view library_id) const override {
    std::shared_lock lock(mutex_);
    const auto it = entries_.find(std::string(library_id));
    if (it == entries_.end()) {
      return std::nullopt;
    }
    return it->second.registration;
  }

  std::shared_ptr<Library>
  library(std::string_view library_id) const override {
    std::shared_lock lock(mutex_);
    const auto it = entries_.find(std::string(library_id));
    if (it == entries_.end()) {
      return nullptr;
    }
    return it->second.library;
  }

  std::optional<CapabilitySet>
  capabilities(std::string_view library_id) const override {
    std::shared_lock lock(mutex_);
    const auto it = entries_.find(std::string(library_id));
    if (it == entries_.end()) {
      return std::nullopt;
    }
    return it->second.capabilities;
  }

  std::vector<LibraryRegistration> list() const override {
    std::shared_lock lock(mutex_);
    std::vector<LibraryRegistration> result;
    result.reserve(entries_.size());
    for (const auto &pair : entries_) {
      result.push_back(pair.second.registration);
    }
    std::sort(result.begin(), result.end(),
              [](const LibraryRegistration &a, const LibraryRegistration &b) {
                return a.library_id < b.library_id;
              });
    return result;
  }

  std::vector<LibraryRegistration>
  candidates(CapabilityId capability, bool require_available) const override {
    std::shared_lock lock(mutex_);

    // 5.2.4：索引只按 supported 建立，available 在查询时过滤。
    const auto index_it = capability_index_.find(capability);
    if (index_it == capability_index_.end()) {
      return {};
    }

    std::vector<std::string> picked;
    picked.reserve(index_it->second.size());
    for (const std::string &library_id : index_it->second) {
      const auto entry_it = entries_.find(library_id);
      if (entry_it == entries_.end()) {
        continue;
      }
      const Entry &entry = entry_it->second;
      if (!entry.registration.enabled || entry.stopping) {
        continue;
      }
      if (!entry.capabilities.supports(capability)) {
        continue;
      }
      if (require_available && !entry.capabilities.available(capability)) {
        continue;
      }
      // 熔断打开的实例不进候选（5.2.5 硬过滤第 5 条）。
      if (entry.circuit_open_until_ms > last_now_ms_) {
        continue;
      }
      // 只保存 library_id，排序与读取时再回到 entries_ 取，
      // 避免持有指向容器元素的裸指针（5.2.5 要求的排序信号仍在）。
      picked.push_back(entry.registration.library_id);
    }

    // 5.2.5 排序信号：健康状态 > priority 降序 > 本地优先 > 稳定决胜项。
    const auto &entries = entries_;
    std::sort(picked.begin(), picked.end(),
              [&entries](const std::string &id_a, const std::string &id_b) {
                const Entry &a = entries.at(id_a);
                const Entry &b = entries.at(id_b);
                const int rank_a = health_rank(a.health.state);
                const int rank_b = health_rank(b.health.state);
                if (rank_a != rank_b) {
                  return rank_a < rank_b;
                }
                if (a.registration.priority != b.registration.priority) {
                  return a.registration.priority > b.registration.priority;
                }
                const bool local_a =
                    a.registration.platform == PlatformKind::Local;
                const bool local_b =
                    b.registration.platform == PlatformKind::Local;
                if (local_a != local_b) {
                  return local_a;
                }
                // 稳定决胜项：注册顺序，其次 library_id。
                if (a.registration_order != b.registration_order) {
                  return a.registration_order < b.registration_order;
                }
                return a.registration.library_id < b.registration.library_id;
              });

    std::vector<LibraryRegistration> result;
    result.reserve(picked.size());
    for (const std::string &id : picked) {
      result.push_back(entries.at(id).registration);
    }
    return result;
  }

  HealthSnapshot health(std::string_view library_id) const override {
    std::shared_lock lock(mutex_);
    const auto it = entries_.find(std::string(library_id));
    if (it == entries_.end()) {
      HealthSnapshot snapshot;
      snapshot.state = HealthState::Unavailable;
      snapshot.reason = ErrorCode::NotFound;
      return snapshot;
    }
    return it->second.health;
  }

  Status refresh_capabilities(std::string_view library_id) override {
    std::shared_ptr<Library> library;
    {
      std::shared_lock lock(mutex_);
      const auto it = entries_.find(std::string(library_id));
      if (it == entries_.end()) {
        return Status::failure(Error::make(
            ErrorCode::NotFound,
            "library not registered: " + std::string(library_id)));
      }
      library = it->second.library;
    }

    // 在锁外读能力：capabilities() 可能访问账号状态。
    CapabilitySet refreshed = library->capabilities();

    std::unique_lock lock(mutex_);
    const auto it = entries_.find(std::string(library_id));
    if (it == entries_.end()) {
      return Status::failure(Error::make(
          ErrorCode::NotFound, "library removed during refresh"));
    }
    it->second.capabilities = std::move(refreshed);
    rebuild_index_locked();
    return Status::success();
  }

  Status admit(std::string_view library_id, std::int64_t now_ms) override {
    std::unique_lock lock(mutex_);
    note_now_locked(now_ms);

    const auto it = entries_.find(std::string(library_id));
    if (it == entries_.end()) {
      return Status::failure(Error::make(
          ErrorCode::NotFound,
          "library not registered: " + std::string(library_id)));
    }
    Entry &entry = it->second;

    if (stopping_ || entry.stopping) {
      return Status::failure(Error::make(
          ErrorCode::ProviderUnavailable, "library is shutting down"));
    }
    if (!entry.registration.enabled) {
      return Status::failure(
          Error::make(ErrorCode::ProviderUnavailable, "library is disabled"));
    }
    if (entry.circuit_open_until_ms > now_ms) {
      Error error = Error::make(ErrorCode::ProviderUnavailable,
                                "circuit is open for this library");
      error.retryable = true;
      error.retry_after = entry.circuit_open_until_ms - now_ms;
      error.library_id = entry.registration.library_id;
      return Status::failure(std::move(error));
    }
    if (entry.rate_limited_until_ms > now_ms) {
      Error error = Error::make(ErrorCode::RateLimited,
                                "library is rate limited");
      error.retryable = true;
      error.retry_after = entry.rate_limited_until_ms - now_ms;
      error.library_id = entry.registration.library_id;
      return Status::failure(std::move(error));
    }
    if (entry.in_flight >= entry.registration.limits.max_concurrency) {
      // 5.2.6：超限直接 rate_limited，不排队。
      Error error =
          Error::make(ErrorCode::RateLimited, "library concurrency limit hit");
      error.retryable = true;
      error.retry_after = 50;
      error.library_id = entry.registration.library_id;
      return Status::failure(std::move(error));
    }

    ++entry.in_flight;
    return Status::success();
  }

  void release(std::string_view library_id, const std::optional<Error> &error,
               std::int64_t now_ms) override {
    std::unique_lock lock(mutex_);
    note_now_locked(now_ms);

    const auto it = entries_.find(std::string(library_id));
    if (it == entries_.end()) {
      return;
    }
    Entry &entry = it->second;
    if (entry.in_flight > 0) {
      --entry.in_flight;
    }

    if (!error.has_value()) {
      entry.consecutive_failures = 0;
      entry.circuit_open_until_ms = 0;
      if (entry.health.state != HealthState::Unauthenticated) {
        entry.health.state = HealthState::Ok;
        entry.health.reason.reset();
      }
      entry.health.checked_at = now_ms;
      return;
    }

    entry.health.checked_at = now_ms;

    switch (error->code) {
    case ErrorCode::AuthRequired:
      entry.health.state = HealthState::Unauthenticated;
      entry.health.reason = ErrorCode::AuthRequired;
      return;
    case ErrorCode::RateLimited:
      entry.health.state = HealthState::RateLimited;
      entry.health.reason = ErrorCode::RateLimited;
      if (error->retry_after.has_value()) {
        entry.rate_limited_until_ms = now_ms + *error->retry_after;
      }
      return;
    case ErrorCode::Unsupported:
    case ErrorCode::PermissionDenied:
    case ErrorCode::NotFound:
    case ErrorCode::InvalidRequest:
      // 不是实例故障，不改健康状态也不累计失败。
      return;
    case ErrorCode::Cancelled:
      return;
    default:
      break;
    }

    if (!is_open_circuit_failure(error->code)) {
      return;
    }

    ++entry.consecutive_failures;
    if (entry.consecutive_failures >=
        entry.registration.limits.circuit_failure_threshold) {
      entry.circuit_open_until_ms =
          now_ms + entry.registration.limits.circuit_open_ms;
      entry.health.state = HealthState::Unavailable;
      entry.health.reason = error->code;
    } else if (entry.health.state == HealthState::Ok) {
      entry.health.state = HealthState::Degraded;
      entry.health.reason = error->code;
    }
  }

  void shutdown() noexcept override {
    std::vector<std::shared_ptr<Library>> libraries;
    {
      std::unique_lock lock(mutex_);
      if (stopping_) {
        return;
      }
      stopping_ = true;
      // 逆序关闭（5.2.7）。
      libraries.reserve(entries_.size());
      std::vector<std::pair<int, std::shared_ptr<Library>>> ordered;
      ordered.reserve(entries_.size());
      for (auto &pair : entries_) {
        pair.second.stopping = true;
        ordered.emplace_back(pair.second.registration_order,
                             pair.second.library);
      }
      std::sort(ordered.begin(), ordered.end(),
                [](const auto &a, const auto &b) { return a.first > b.first; });
      for (auto &item : ordered) {
        libraries.push_back(std::move(item.second));
      }
      entries_.clear();
      capability_index_.clear();
    }

    for (auto &library : libraries) {
      if (library) {
        library->close();
      }
    }
  }

private:
  void note_now_locked(std::int64_t now_ms) {
    if (now_ms > last_now_ms_) {
      last_now_ms_ = now_ms;
    }
  }

  // 5.2.4：能力倒排索引只按 supported 建立。
  void rebuild_index_locked() {
    capability_index_.clear();
    for (const auto &pair : entries_) {
      const Entry &entry = pair.second;
      for (const CapabilityState &state : entry.capabilities.items) {
        if (state.supported) {
          capability_index_[state.id].push_back(entry.registration.library_id);
        }
      }
    }
  }

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::map<CapabilityId, std::vector<std::string>> capability_index_;
  bool stopping_ = false;
  int next_order_ = 0;
  std::int64_t last_now_ms_ = 0;
};

} // namespace

std::shared_ptr<LibraryRegistry> make_library_registry() {
  return std::make_shared<LibraryRegistryImpl>();
}

} // namespace media_library
