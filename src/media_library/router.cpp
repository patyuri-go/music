#include "media_library/router.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace media_library {
namespace {

// 5.3.3：每个方面最多尝试的候选数。
constexpr std::size_t kMaxCandidates = 3;

std::int64_t current_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 5.3.10：全部来源失败时的整体错误码优先级（数字越小越优先）。
int aggregate_error_rank(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::PermissionDenied:
    return 0;
  case ErrorCode::AuthRequired:
    return 1;
  case ErrorCode::RateLimited:
    return 2;
  case ErrorCode::ProviderUnavailable:
    return 3;
  case ErrorCode::Timeout:
    return 4;
  default:
    return 5;
  }
}

// admit / release 必须配对。错误路径忘记 release 会让该库的并发名额永久
// 被占用，所以用 RAII 兜底；正常完成时由 finish() 显式给出结果。
class AdmissionGuard {
public:
  AdmissionGuard(std::shared_ptr<LibraryRegistry> registry, std::string library_id)
      : registry_(std::move(registry)), library_id_(std::move(library_id)) {}

  ~AdmissionGuard() {
    if (!finished_) {
      registry_->release(library_id_, std::nullopt, current_ms());
    }
  }

  AdmissionGuard(const AdmissionGuard &) = delete;
  AdmissionGuard &operator=(const AdmissionGuard &) = delete;

  void finish(const std::optional<Error> &error) {
    if (finished_) {
      return;
    }
    finished_ = true;
    registry_->release(library_id_, error, current_ms());
  }

private:
  std::shared_ptr<LibraryRegistry> registry_;
  std::string library_id_;
  bool finished_ = false;
};

class AggregatePriorityImpl final : public AggregatePriority {
public:
  std::vector<std::string> order() const override {
    std::shared_lock lock(mutex_);
    return order_;
  }

  Status set_order(std::vector<std::string> library_ids) override {
    for (const std::string &id : library_ids) {
      if (id.empty()) {
        return Status::failure(
            Error::make(ErrorCode::InvalidRequest, "empty library id"));
      }
    }
    std::unique_lock lock(mutex_);
    order_ = std::move(library_ids);
    ++revision_;
    return Status::success();
  }

  std::uint64_t revision() const override {
    std::shared_lock lock(mutex_);
    return revision_;
  }

private:
  mutable std::shared_mutex mutex_;
  std::vector<std::string> order_;
  std::uint64_t revision_ = 0;
};

class RegistryBuiltinOrder final : public BuiltinOrderSource {
public:
  explicit RegistryBuiltinOrder(std::shared_ptr<LibraryRegistry> registry)
      : registry_(std::move(registry)) {}

  std::vector<std::string> order() const override {
    std::vector<std::string> ids;
    if (registry_ == nullptr) {
      return ids;
    }
    std::vector<LibraryRegistration> items = registry_->list();
    std::sort(items.begin(), items.end(),
              [](const LibraryRegistration &a, const LibraryRegistration &b) {
                const bool a_local = a.platform == PlatformKind::Local;
                const bool b_local = b.platform == PlatformKind::Local;
                if (a_local != b_local) {
                  return a_local;
                }
                if (a.priority != b.priority) {
                  return a.priority > b.priority;
                }
                return a.library_id < b.library_id;
              });
    ids.reserve(items.size());
    for (const LibraryRegistration &item : items) {
      ids.push_back(item.library_id);
    }
    return ids;
  }

private:
  std::shared_ptr<LibraryRegistry> registry_;
};

} // namespace

std::string_view source_state_name(SourceState state) noexcept {
  switch (state) {
  case SourceState::Ok:
    return "ok";
  case SourceState::Partial:
    return "partial";
  case SourceState::Unavailable:
    return "unavailable";
  case SourceState::Unauthenticated:
    return "unauthenticated";
  case SourceState::RateLimited:
    return "rate_limited";
  case SourceState::Timeout:
    return "timeout";
  case SourceState::Unsupported:
    return "unsupported";
  }
  return "unavailable";
}

// 5.3.5：state 与 ErrorCode 的映射集中在这里，别处不要各写一份。
SourceState source_state_from(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::AuthRequired:
    return SourceState::Unauthenticated;
  case ErrorCode::RateLimited:
    return SourceState::RateLimited;
  case ErrorCode::Timeout:
    return SourceState::Timeout;
  case ErrorCode::Unsupported:
    return SourceState::Unsupported;
  case ErrorCode::InvalidRequest:
  case ErrorCode::NotFound:
  case ErrorCode::PermissionDenied:
  case ErrorCode::ProviderUnavailable:
  case ErrorCode::PlaybackExpired:
  case ErrorCode::Conflict:
  case ErrorCode::Cancelled:
  case ErrorCode::Internal:
    return SourceState::Unavailable;
  }
  return SourceState::Unavailable;
}

std::shared_ptr<AggregatePriority> make_memory_aggregate_priority() {
  return std::make_shared<AggregatePriorityImpl>();
}

std::shared_ptr<BuiltinOrderSource>
make_registry_builtin_order(std::shared_ptr<LibraryRegistry> registry) {
  return std::make_shared<RegistryBuiltinOrder>(std::move(registry));
}

Router::Router(std::shared_ptr<LibraryRegistry> registry,
               std::shared_ptr<EntityLinkResolver> links,
               std::shared_ptr<AspectPriority> aspect_priority,
               std::shared_ptr<AggregatePriority> aggregate_priority)
    : registry_(std::move(registry)), links_(std::move(links)),
      aspect_priority_(std::move(aspect_priority)),
      aggregate_priority_(std::move(aggregate_priority)) {}

std::shared_ptr<Router>
make_router(std::shared_ptr<LibraryRegistry> registry,
            std::shared_ptr<EntityLinkResolver> links,
            std::shared_ptr<AspectPriority> aspect_priority,
            std::shared_ptr<AggregatePriority> aggregate_priority) {
  return std::make_shared<Router>(std::move(registry), std::move(links),
                                  std::move(aspect_priority),
                                  std::move(aggregate_priority));
}

std::int64_t Router::now_ms() const { return current_ms(); }

// 5.3.3：只有这些错误值得换下一个候选。
// permission_denied / auth_required / unsupported 换库只会变成“绕过限制”。
bool Router::should_fall_back(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::ProviderUnavailable:
  case ErrorCode::Timeout:
  case ErrorCode::RateLimited:
  case ErrorCode::PlaybackExpired:
  case ErrorCode::NotFound:
    return true;
  case ErrorCode::InvalidRequest:
  case ErrorCode::AuthRequired:
  case ErrorCode::PermissionDenied:
  case ErrorCode::Unsupported:
  case ErrorCode::Conflict:
  case ErrorCode::Cancelled:
  case ErrorCode::Internal:
    return false;
  }
  return false;
}

Result<std::shared_ptr<Library>>
Router::acquire(const std::string &library_id,
                CapabilityId capability) const {
  if (library_id.empty()) {
    return Result<std::shared_ptr<Library>>::failure(Error::make(
        ErrorCode::InvalidRequest, "library id is required"));
  }
  if (registry_ == nullptr) {
    return Result<std::shared_ptr<Library>>::failure(Error::make(
        ErrorCode::ProviderUnavailable, "library registry is not configured"));
  }

  const auto registration = registry_->find(library_id);
  if (!registration.has_value()) {
    Error error = Error::make(ErrorCode::InvalidRequest,
                              "unknown library: " + library_id);
    error.library_id = library_id;
    return Result<std::shared_ptr<Library>>::failure(std::move(error));
  }

  std::shared_ptr<Library> library = registry_->library(library_id);
  if (library == nullptr) {
    Error error = Error::make(ErrorCode::ProviderUnavailable,
                              "library is not available: " + library_id);
    error.library_id = library_id;
    return Result<std::shared_ptr<Library>>::failure(std::move(error));
  }

  const auto capabilities = registry_->capabilities(library_id);
  if (!capabilities.has_value() ||
      !capabilities->supports(capability)) {
    Error error = Error::make(ErrorCode::Unsupported,
                              "library does not support the requested capability");
    error.library_id = library_id;
    error.capability = std::string(capability_name(capability));
    return Result<std::shared_ptr<Library>>::failure(std::move(error));
  }

  // 5.2.6：并发上限、熔断、限流在这里统一拦截。
  const Status admitted = registry_->admit(library_id, now_ms());
  if (!admitted.ok()) {
    return Result<std::shared_ptr<Library>>::failure(admitted.error());
  }
  return Result<std::shared_ptr<Library>>::success(std::move(library));
}

// ---------------------------------------------------------------------------
// 5.3.2 单源路由：调用方已经指定 library_id。
// 不读任何优先级、不做替换、不回退、不返回 sources。
// ---------------------------------------------------------------------------

Result<Track> Router::get_track(const TrackRef &ref, const CallContext &ctx) {
  auto library = acquire(ref.library_id, CapabilityId::CatalogRead);
  if (!library.ok()) {
    return Result<Track>::failure(library.error());
  }
  AdmissionGuard guard(registry_, ref.library_id);

  auto reader = library.value()->capability<CatalogReader>();
  if (!reader.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no catalog reader");
    error.library_id = ref.library_id;
    guard.finish(error);
    return Result<Track>::failure(std::move(error));
  }

  Result<Track> result = reader->get().get_track(ref, ctx);
  if (!result.ok()) {
    guard.finish(result.error());
    return result;
  }
  guard.finish(std::nullopt);
  return result;
}

Result<Album> Router::get_album(const AlbumRef &ref, const CallContext &ctx) {
  auto library = acquire(ref.library_id, CapabilityId::CatalogRead);
  if (!library.ok()) {
    return Result<Album>::failure(library.error());
  }
  AdmissionGuard guard(registry_, ref.library_id);

  auto reader = library.value()->capability<CatalogReader>();
  if (!reader.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no catalog reader");
    error.library_id = ref.library_id;
    guard.finish(error);
    return Result<Album>::failure(std::move(error));
  }

  Result<Album> result = reader->get().get_album(ref, ctx);
  if (!result.ok()) {
    guard.finish(result.error());
    return result;
  }
  guard.finish(std::nullopt);
  return result;
}

Result<Artist> Router::get_artist(const ArtistRef &ref,
                                  const CallContext &ctx) {
  auto library = acquire(ref.library_id, CapabilityId::CatalogRead);
  if (!library.ok()) {
    return Result<Artist>::failure(library.error());
  }
  AdmissionGuard guard(registry_, ref.library_id);

  auto reader = library.value()->capability<CatalogReader>();
  if (!reader.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no catalog reader");
    error.library_id = ref.library_id;
    guard.finish(error);
    return Result<Artist>::failure(std::move(error));
  }

  Result<Artist> result = reader->get().get_artist(ref, ctx);
  if (!result.ok()) {
    guard.finish(result.error());
    return result;
  }
  guard.finish(std::nullopt);
  return result;
}

Result<Page<Track>> Router::list_album_tracks(const AlbumRef &ref,
                                              const PageRequest &page,
                                              const CallContext &ctx) {
  auto library = acquire(ref.library_id, CapabilityId::CatalogRead);
  if (!library.ok()) {
    return Result<Page<Track>>::failure(library.error());
  }
  AdmissionGuard guard(registry_, ref.library_id);

  auto reader = library.value()->capability<CatalogReader>();
  if (!reader.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no catalog reader");
    error.library_id = ref.library_id;
    guard.finish(error);
    return Result<Page<Track>>::failure(std::move(error));
  }

  Result<Page<Track>> result = reader->get().list_album_tracks(ref, page, ctx);
  if (!result.ok()) {
    guard.finish(result.error());
    return result;
  }
  guard.finish(std::nullopt);
  return result;
}

Result<Page<Album>> Router::list_artist_albums(const ArtistRef &ref,
                                               const PageRequest &page,
                                               const CallContext &ctx) {
  auto library = acquire(ref.library_id, CapabilityId::CatalogRead);
  if (!library.ok()) {
    return Result<Page<Album>>::failure(library.error());
  }
  AdmissionGuard guard(registry_, ref.library_id);

  auto reader = library.value()->capability<CatalogReader>();
  if (!reader.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no catalog reader");
    error.library_id = ref.library_id;
    guard.finish(error);
    return Result<Page<Album>>::failure(std::move(error));
  }

  Result<Page<Album>> result =
      reader->get().list_artist_albums(ref, page, ctx);
  if (!result.ok()) {
    guard.finish(result.error());
    return result;
  }
  guard.finish(std::nullopt);
  return result;
}

// 5.3.2：指定库的搜索只可能来自该库，命中 0 条就是 0 条，绝不从别处补。
Result<Page<SearchHit>> Router::search_in(const std::string &library_id,
                                          const SearchQuery &query,
                                          const CallContext &ctx) {
  auto library = acquire(library_id, CapabilityId::Search);
  if (!library.ok()) {
    return Result<Page<SearchHit>>::failure(library.error());
  }
  AdmissionGuard guard(registry_, library_id);

  auto searcher = library.value()->capability<Searcher>();
  if (!searcher.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no searcher");
    error.library_id = library_id;
    guard.finish(error);
    return Result<Page<SearchHit>>::failure(std::move(error));
  }

  Result<Page<SearchHit>> result = searcher->get().search(query, ctx);
  if (!result.ok()) {
    guard.finish(result.error());
    return result;
  }
  guard.finish(std::nullopt);
  return result;
}

// ---------------------------------------------------------------------------
// 5.3.3 多源路由
// ---------------------------------------------------------------------------

std::vector<SourceRef>
Router::candidates_for(const SourceRef &ref, Aspect aspect,
                       const CallContext &ctx) const {
  std::vector<SourceRef> result;
  if (registry_ == nullptr || links_ == nullptr) {
    return result;
  }

  // 查不到映射 -> 退化为单源，就用调用方给的那个引用（不是错误）。
  const Result<EntityLinkId> link = links_->link_of(ref, ctx);
  if (!link.ok()) {
    result.push_back(ref);
    return result;
  }

  const Result<std::vector<LinkedRef>> refs = links_->refs_of(link.value(), ctx);
  if (!refs.ok()) {
    result.push_back(ref);
    return result;
  }

  // 只保留同实体类型的引用：artist 与 album_artist 即使指向同一个底层
  // 艺术家也是不同 entity_type，不能落到同一个候选池。
  std::vector<SourceRef> pool;
  for (const LinkedRef &item : refs.value()) {
    if (item.ref.entity_type != ref.entity_type) {
      continue;
    }
    if (!aspect_accepts(aspect, item.ref.entity_type)) {
      continue;
    }
    pool.push_back(item.ref);
  }
  if (pool.empty()) {
    return result;
  }

  // 硬过滤：enabled / supported / available / 未熔断。
  const CapabilityId capability = capability_for(aspect);
  const std::vector<LibraryRegistration> usable =
      registry_->candidates(capability, true);
  std::unordered_set<std::string> usable_ids;
  usable_ids.reserve(usable.size());
  for (const LibraryRegistration &registration : usable) {
    usable_ids.insert(registration.library_id);
  }

  // 三级回落只在 resolver 的 effective_order 里算一次。
  const std::vector<std::string> order =
      links_->effective_order(link.value(), aspect);
  for (const std::string &candidate_id : order) {
    if (usable_ids.count(candidate_id) == 0) {
      continue;
    }
    for (const SourceRef &candidate : pool) {
      if (candidate.library_id == candidate_id) {
        result.push_back(candidate);
        break;
      }
    }
  }

  if (result.size() > kMaxCandidates) {
    result.resize(kMaxCandidates);
  }
  return result;
}

Result<SourceRef> Router::resolve(const SourceRef &ref, Aspect aspect,
                                  const CallContext &ctx) const {
  const std::vector<SourceRef> candidates = candidates_for(ref, aspect, ctx);
  if (candidates.empty()) {
    Error error = Error::make(
        ErrorCode::Unsupported,
        "no library provides aspect " + std::string(aspect_name(aspect)));
    error.library_id = ref.library_id;
    error.capability = std::string(capability_name(capability_for(aspect)));
    return Result<SourceRef>::failure(std::move(error));
  }
  return Result<SourceRef>::success(candidates.front());
}

std::vector<std::string> Router::order_of(const SourceRef &ref,
                                          Aspect aspect) const {
  if (links_ == nullptr) {
    return {ref.library_id};
  }
  const CallContext ctx;
  const Result<EntityLinkId> link = links_->link_of(ref, ctx);
  if (!link.ok()) {
    return {ref.library_id};
  }
  return links_->effective_order(link.value(), aspect);
}

// 多源执行：按候选顺序逐个尝试；失败时只有 5.3.3 列出的错误才换下一个。
template <typename T, typename Executor>
Result<AspectResult<T>>
Router::run_aspect(const SourceRef &ref, Aspect aspect, const CallContext &ctx,
                   Executor &&executor) {
  const std::vector<SourceRef> candidates = candidates_for(ref, aspect, ctx);
  if (candidates.empty()) {
    Error error = Error::make(
        ErrorCode::Unsupported,
        "no library provides aspect " + std::string(aspect_name(aspect)));
    error.library_id = ref.library_id;
    error.capability = std::string(capability_name(capability_for(aspect)));
    return Result<AspectResult<T>>::failure(std::move(error));
  }

  AspectResult<T> output;
  std::optional<Error> last_error;

  for (const SourceRef &candidate : candidates) {
    if (ctx.deadline_ms.has_value() && current_ms() >= *ctx.deadline_ms) {
      last_error = Error::make(ErrorCode::Timeout, "request deadline exceeded");
      break;
    }

    const std::int64_t started = current_ms();
    AspectAttempt attempt;
    attempt.library_id = candidate.library_id;

    auto library = acquire(candidate.library_id, capability_for(aspect));
    if (!library.ok()) {
      attempt.ok = false;
      attempt.error = library.error().code;
      attempt.elapsed_ms = current_ms() - started;
      output.attempts.push_back(attempt);
      last_error = library.error();
      if (!should_fall_back(library.error().code)) {
        break;
      }
      continue;
    }

    AdmissionGuard guard(registry_, candidate.library_id);
    Result<T> result = executor(*library.value(), candidate, ctx);
    attempt.elapsed_ms = current_ms() - started;

    if (result.ok()) {
      guard.finish(std::nullopt);
      attempt.ok = true;
      output.attempts.push_back(attempt);
      output.value = std::move(result).value();
      output.source = candidate;
      output.substituted = !(candidate == ref);
      return Result<AspectResult<T>>::success(std::move(output));
    }

    guard.finish(result.error());
    attempt.ok = false;
    attempt.error = result.error().code;
    output.attempts.push_back(attempt);
    last_error = result.error();
    if (!should_fall_back(result.error().code)) {
      break;
    }
  }

  if (!last_error.has_value()) {
    last_error = Error::make(ErrorCode::Unsupported,
                             "no candidate produced a value");
  }
  return Result<AspectResult<T>>::failure(std::move(*last_error));
}

Result<AspectResult<Track>> Router::resolve_track(const TrackRef &ref,
                                                  const CallContext &ctx) {
  return run_aspect<Track>(
      ref, Aspect::Metadata, ctx,
      [](Library &library, const SourceRef &candidate,
         const CallContext &call_ctx) -> Result<Track> {
        auto reader = library.capability<CatalogReader>();
        if (!reader.has_value()) {
          return Result<Track>::failure(
              Error::make(ErrorCode::Unsupported, "no catalog reader"));
        }
        return reader->get().get_track(candidate, call_ctx);
      });
}

Result<AspectResult<Lyrics>> Router::resolve_lyrics(const TrackRef &ref,
                                                    const CallContext &ctx) {
  return run_aspect<Lyrics>(
      ref, Aspect::Lyrics, ctx,
      [](Library &library, const SourceRef &candidate,
         const CallContext &call_ctx) -> Result<Lyrics> {
        auto provider = library.capability<LyricsProvider>();
        if (!provider.has_value()) {
          return Result<Lyrics>::failure(
              Error::make(ErrorCode::Unsupported, "no lyrics provider"));
        }
        return provider->get().get_lyrics(candidate, call_ctx);
      });
}

Result<AspectResult<PlaybackSource>>
Router::resolve_playback(const TrackRef &ref, const CallContext &ctx) {
  return run_aspect<PlaybackSource>(
      ref, Aspect::Stream, ctx,
      [](Library &library, const SourceRef &candidate,
         const CallContext &call_ctx) -> Result<PlaybackSource> {
        auto resolver = library.capability<StreamResolver>();
        if (!resolver.has_value()) {
          return Result<PlaybackSource>::failure(
              Error::make(ErrorCode::Unsupported, "no stream resolver"));
        }
        return resolver->get().resolve_playback(candidate, call_ctx);
      });
}

Result<AspectResult<ArtworkRef>> Router::resolve_artwork(const SourceRef &owner,
                                                         const CallContext &ctx) {
  return run_aspect<ArtworkRef>(
      owner, Aspect::Artwork, ctx,
      [](Library &library, const SourceRef &candidate,
         const CallContext &call_ctx) -> Result<ArtworkRef> {
        auto provider = library.capability<ArtworkProvider>();
        if (!provider.has_value()) {
          return Result<ArtworkRef>::failure(
              Error::make(ErrorCode::Unsupported, "no artwork provider"));
        }
        return provider->get().get_artwork(candidate, call_ctx);
      });
}

// ---------------------------------------------------------------------------
// 5.3.4 多源聚合
// 本轮只做第一页：next_cursor 恒为空，has_more 表示来源还有更多。
// ---------------------------------------------------------------------------

Result<AggregatedPage<SearchHit>> Router::search(const SearchQuery &query,
                                                 const CallContext &ctx) {
  AggregatedPage<SearchHit> page;
  if (registry_ == nullptr) {
    return Result<AggregatedPage<SearchHit>>::success(std::move(page));
  }

  // 候选为空不是错误：返回空结果 + 全部来源状态（这里状态也是空的）。
  std::vector<LibraryRegistration> candidates =
      registry_->candidates(CapabilityId::Search, true);
  if (candidates.empty()) {
    return Result<AggregatedPage<SearchHit>>::success(std::move(page));
  }

  // 5.3.9 聚合优先级：高优先级库整组在前。未列出的库追加末尾。
  std::vector<std::string> order;
  if (aggregate_priority_ != nullptr) {
    order = aggregate_priority_->order();
  }
  std::unordered_map<std::string, std::size_t> tier_of;
  for (std::size_t i = 0; i < order.size(); ++i) {
    tier_of.emplace(order[i], i);
  }
  std::size_t next_tier = order.size();
  for (const LibraryRegistration &registration : candidates) {
    if (tier_of.count(registration.library_id) == 0) {
      tier_of.emplace(registration.library_id, next_tier++);
    }
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [&tier_of](const LibraryRegistration &a,
                              const LibraryRegistration &b) {
                     return tier_of.at(a.library_id) <
                            tier_of.at(b.library_id);
                   });

  const int limit = query.page.limit > 0 ? query.page.limit : 50;

  std::vector<std::pair<std::size_t, SearchHit>> collected;
  std::vector<Error> failures;
  bool any_success = false;
  bool any_source_has_more = false;

  for (const LibraryRegistration &registration : candidates) {
    SourceStatus status;
    status.library_id = registration.library_id;
    const std::int64_t started = current_ms();

    auto library = acquire(registration.library_id, CapabilityId::Search);
    if (!library.ok()) {
      status.state = source_state_from(library.error().code);
      status.error = library.error();
      status.elapsed_ms = current_ms() - started;
      page.sources.push_back(status);
      failures.push_back(library.error());
      continue;
    }

    AdmissionGuard guard(registry_, registration.library_id);
    auto searcher = library.value()->capability<Searcher>();
    if (!searcher.has_value()) {
      Error error = Error::make(ErrorCode::Unsupported, "no searcher");
      error.library_id = registration.library_id;
      guard.finish(error);
      status.state = source_state_from(error.code);
      status.error = error;
      status.elapsed_ms = current_ms() - started;
      page.sources.push_back(status);
      failures.push_back(std::move(error));
      continue;
    }

    Result<Page<SearchHit>> result = searcher->get().search(query, ctx);
    status.elapsed_ms = current_ms() - started;
    if (!result.ok()) {
      guard.finish(result.error());
      status.state = source_state_from(result.error().code);
      status.error = result.error();
      page.sources.push_back(status);
      failures.push_back(result.error());
      continue;
    }

    guard.finish(std::nullopt);
    status.state = SourceState::Ok;
    status.item_count = static_cast<int>(result.value().items.size());
    page.sources.push_back(status);
    any_success = true;
    any_source_has_more = any_source_has_more || result.value().has_more;

    const std::size_t tier = tier_of.at(registration.library_id);
    for (SearchHit &hit : result.value().items) {
      collected.emplace_back(tier, std::move(hit));
    }
  }

  // 全部来源失败且没有任何结果 -> 整体错误（5.3.10）。
  if (!any_success && collected.empty() && !failures.empty()) {
    // 用下标而非指针挑选最优错误，避免指向容器元素的裸指针。
    std::size_t chosen = 0;
    for (std::size_t i = 1; i < failures.size(); ++i) {
      if (aggregate_error_rank(failures[i].code) <
          aggregate_error_rank(failures[chosen].code)) {
        chosen = i;
      }
    }
    return Result<AggregatedPage<SearchHit>>::failure(failures[chosen]);
  }

  // 5.3.8 统一排序：第一层是聚合优先级（库分组），第二层是组内信号。
  // 排序不依赖响应到达顺序，且以 SourceRef 字符串作稳定决胜项。
  std::stable_sort(
      collected.begin(), collected.end(),
      [](const std::pair<std::size_t, SearchHit> &a,
         const std::pair<std::size_t, SearchHit> &b) {
        if (a.first != b.first) {
          return a.first < b.first;
        }
        if (a.second.playable != b.second.playable) {
          return a.second.playable;
        }
        if (a.second.title != b.second.title) {
          return a.second.title < b.second.title;
        }
        return to_string(a.second.ref) < to_string(b.second.ref);
      });

  const std::size_t total = collected.size();
  const std::size_t take = std::min<std::size_t>(
      total, static_cast<std::size_t>(limit));
  page.items.reserve(take);
  for (std::size_t i = 0; i < take; ++i) {
    page.items.push_back(std::move(collected[i].second));
  }
  page.has_more = total > take || any_source_has_more;
  return Result<AggregatedPage<SearchHit>>::success(std::move(page));
}

} // namespace media_library
