#include "media_library/router.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string_view>
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

// 搜索分页参数上限。offset 上限与 Subsonic 门面一致，防止一次重放
// 把上游拉穿。
constexpr int kMaxSearchLimit = 200;
constexpr int kMaxSearchOffset = 100000;
// 单次向来源请求的条数。网易云内部会把 limit clamp 到 100。
constexpr int kSourcePageSize = 100;

void hash_mix(std::uint64_t &state, std::string_view value) {
  for (const unsigned char byte : value) {
    state ^= byte;
    state *= 1099511628211ULL;
  }
  state ^= 0xffU;
  state *= 1099511628211ULL;
}

// 游标只对同一组查询条件 + 同一组候选库顺序有效。条件或顺序变了就
// 返回 invalid_request，让客户端重新搜索。
std::uint64_t search_revision(
    const SearchQuery &query,
    const std::vector<LibraryRegistration> &candidates) {
  std::uint64_t state = 1469598103934665603ULL;
  hash_mix(state, query.text);
  for (const EntityType type : query.types) {
    hash_mix(state, std::to_string(static_cast<int>(type)));
  }
  for (const std::string &artist : query.artists) {
    hash_mix(state, artist);
  }
  for (const std::string &artist : query.album_artists) {
    hash_mix(state, artist);
  }
  hash_mix(state, query.library_id.value_or(std::string()));
  hash_mix(state, query.locale.value_or(std::string()));
  hash_mix(state, query.include_unplayable ? "1" : "0");
  for (const LibraryRegistration &candidate : candidates) {
    hash_mix(state, candidate.library_id);
  }
  return state;
}

std::string encode_search_cursor(int offset, std::uint64_t revision) {
  return "rs1:" + std::to_string(offset) + ":" + std::to_string(revision);
}

bool parse_u64(std::string_view text, std::uint64_t &value) {
  if (text.empty()) {
    return false;
  }
  const char *begin = text.data();
  const char *end = begin + text.size();
  const auto parsed = std::from_chars(begin, end, value);
  return parsed.ec == std::errc() && parsed.ptr == end;
}

bool decode_search_cursor(std::string_view text, int &offset,
                          std::uint64_t &revision) {
  constexpr std::string_view prefix = "rs1:";
  if (text.size() <= prefix.size() ||
      text.substr(0, prefix.size()) != prefix) {
    return false;
  }
  text.remove_prefix(prefix.size());
  const std::size_t separator = text.find(':');
  if (separator == std::string_view::npos) {
    return false;
  }
  std::uint64_t parsed_offset = 0;
  std::uint64_t parsed_revision = 0;
  if (!parse_u64(text.substr(0, separator), parsed_offset) ||
      !parse_u64(text.substr(separator + 1), parsed_revision)) {
    return false;
  }
  if (parsed_offset > static_cast<std::uint64_t>(kMaxSearchOffset)) {
    return false;
  }
  offset = static_cast<int>(parsed_offset);
  revision = parsed_revision;
  return true;
}

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
  case SourceState::Skipped:
    return "skipped";
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

// ---------------------------------------------------------------------------
// 5.3.2 单源路由：调用方已经指定 library_id。
// 不读任何优先级、不做替换、不回退、不返回 sources。
// ---------------------------------------------------------------------------

Result<Track> Router::get_track(const TrackRef &ref, const CallContext &ctx) {
  auto lease = registry_->acquire(ref.library_id, CapabilityId::CatalogRead, now_ms(),
                              ctx.deadline_ms);
  if (!lease.ok()) {
    return Result<Track>::failure(lease.error());
  }
  auto library = lease.value().library();

  auto reader = library->capability<CatalogReader>();
  if (!reader.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no catalog reader");
    error.library_id = ref.library_id;
    lease.value().finish(error);
    return Result<Track>::failure(std::move(error));
  }

  Result<Track> result = reader->get().get_track(ref, ctx);
  if (!result.ok()) {
    lease.value().finish(result.error());
    return result;
  }
  lease.value().finish(std::nullopt);
  return result;
}

Result<Album> Router::get_album(const AlbumRef &ref, const CallContext &ctx) {
  auto lease = registry_->acquire(ref.library_id, CapabilityId::CatalogRead, now_ms(),
                              ctx.deadline_ms);
  if (!lease.ok()) {
    return Result<Album>::failure(lease.error());
  }
  auto library = lease.value().library();

  auto reader = library->capability<CatalogReader>();
  if (!reader.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no catalog reader");
    error.library_id = ref.library_id;
    lease.value().finish(error);
    return Result<Album>::failure(std::move(error));
  }

  Result<Album> result = reader->get().get_album(ref, ctx);
  if (!result.ok()) {
    lease.value().finish(result.error());
    return result;
  }
  lease.value().finish(std::nullopt);
  return result;
}

Result<Artist> Router::get_artist(const ArtistRef &ref,
                                  const CallContext &ctx) {
  auto lease = registry_->acquire(ref.library_id, CapabilityId::CatalogRead, now_ms(),
                              ctx.deadline_ms);
  if (!lease.ok()) {
    return Result<Artist>::failure(lease.error());
  }
  auto library = lease.value().library();

  auto reader = library->capability<CatalogReader>();
  if (!reader.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no catalog reader");
    error.library_id = ref.library_id;
    lease.value().finish(error);
    return Result<Artist>::failure(std::move(error));
  }

  Result<Artist> result = reader->get().get_artist(ref, ctx);
  if (!result.ok()) {
    lease.value().finish(result.error());
    return result;
  }
  lease.value().finish(std::nullopt);
  return result;
}

Result<Page<Track>> Router::list_album_tracks(const AlbumRef &ref,
                                              const PageRequest &page,
                                              const CallContext &ctx) {
  auto lease = registry_->acquire(ref.library_id, CapabilityId::CatalogRead, now_ms(),
                              ctx.deadline_ms);
  if (!lease.ok()) {
    return Result<Page<Track>>::failure(lease.error());
  }
  auto library = lease.value().library();

  auto reader = library->capability<CatalogReader>();
  if (!reader.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no catalog reader");
    error.library_id = ref.library_id;
    lease.value().finish(error);
    return Result<Page<Track>>::failure(std::move(error));
  }

  Result<Page<Track>> result = reader->get().list_album_tracks(ref, page, ctx);
  if (!result.ok()) {
    lease.value().finish(result.error());
    return result;
  }
  lease.value().finish(std::nullopt);
  return result;
}

Result<Page<Album>> Router::list_artist_albums(const ArtistRef &ref,
                                               const PageRequest &page,
                                               const CallContext &ctx) {
  auto lease = registry_->acquire(ref.library_id, CapabilityId::CatalogRead, now_ms(),
                              ctx.deadline_ms);
  if (!lease.ok()) {
    return Result<Page<Album>>::failure(lease.error());
  }
  auto library = lease.value().library();

  auto reader = library->capability<CatalogReader>();
  if (!reader.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no catalog reader");
    error.library_id = ref.library_id;
    lease.value().finish(error);
    return Result<Page<Album>>::failure(std::move(error));
  }

  Result<Page<Album>> result =
      reader->get().list_artist_albums(ref, page, ctx);
  if (!result.ok()) {
    lease.value().finish(result.error());
    return result;
  }
  lease.value().finish(std::nullopt);
  return result;
}

// 歌单读取走 PlaylistRead 能力，不走 CatalogReader。
Result<Playlist> Router::get_playlist(const PlaylistRef &ref,
                                      const CallContext &ctx) {
  auto lease = registry_->acquire(ref.library_id, CapabilityId::PlaylistRead, now_ms(),
                              ctx.deadline_ms);
  if (!lease.ok()) {
    return Result<Playlist>::failure(lease.error());
  }
  auto library = lease.value().library();

  auto reader = library->capability<PlaylistReader>();
  if (!reader.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no playlist reader");
    error.library_id = ref.library_id;
    lease.value().finish(error);
    return Result<Playlist>::failure(std::move(error));
  }

  Result<Playlist> result = reader->get().get_playlist(ref, ctx);
  if (!result.ok()) {
    lease.value().finish(result.error());
    return result;
  }
  lease.value().finish(std::nullopt);
  return result;
}

Result<Page<Playlist>> Router::list_playlists(const std::string &library_id,
                                              const PageRequest &page,
                                              const CallContext &ctx) {
  auto lease = registry_->acquire(library_id, CapabilityId::PlaylistRead, now_ms(),
                              ctx.deadline_ms);
  if (!lease.ok()) {
    return Result<Page<Playlist>>::failure(lease.error());
  }
  auto library = lease.value().library();

  auto reader = library->capability<PlaylistReader>();
  if (!reader.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no playlist reader");
    error.library_id = library_id;
    lease.value().finish(error);
    return Result<Page<Playlist>>::failure(std::move(error));
  }

  Result<Page<Playlist>> result = reader->get().list_playlists(page, ctx);
  if (!result.ok()) {
    lease.value().finish(result.error());
    return result;
  }
  lease.value().finish(std::nullopt);
  return result;
}

// 5.3.2：指定库的搜索只可能来自该库，命中 0 条就是 0 条，绝不从别处补。
Result<Page<SearchHit>> Router::search_in(const std::string &library_id,
                                          const SearchQuery &query,
                                          const CallContext &ctx) {
  auto lease = registry_->acquire(library_id, CapabilityId::Search, now_ms(),
                              ctx.deadline_ms);
  if (!lease.ok()) {
    return Result<Page<SearchHit>>::failure(lease.error());
  }
  auto library = lease.value().library();

  auto searcher = library->capability<Searcher>();
  if (!searcher.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no searcher");
    error.library_id = library_id;
    lease.value().finish(error);
    return Result<Page<SearchHit>>::failure(std::move(error));
  }

  Result<Page<SearchHit>> result = searcher->get().search(query, ctx);
  if (!result.ok()) {
    lease.value().finish(result.error());
    return result;
  }
  lease.value().finish(std::nullopt);
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
  const Result<EntityLinkView> link = links_->find_link(ref, ctx);
  if (!link.ok()) {
    result.push_back(ref);
    return result;
  }

  // 只保留同实体类型的引用：artist 与 album_artist 即使指向同一个底层
  // 艺术家也是不同 entity_type，不能落到同一个候选池。
  std::vector<SourceRef> pool;
  for (const LinkedRef &item : link.value().refs) {
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
      links_->effective_order(ref, aspect);
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
  const Result<EntityLinkView> link = links_->find_link(ref, ctx);
  if (!link.ok()) {
    return {ref.library_id};
  }
  return links_->effective_order(ref, aspect);
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

    auto lease = registry_->acquire(candidate.library_id, capability_for(aspect), now_ms(),
                           ctx.deadline_ms);
    if (!lease.ok()) {
      attempt.ok = false;
      attempt.error = lease.error().code;
      attempt.elapsed_ms = current_ms() - started;
      output.attempts.push_back(attempt);
      last_error = lease.error();
      if (!should_fall_back(lease.error().code)) {
        break;
      }
      continue;
    }

    auto library = lease.value().library();
    Result<T> result = executor(*library, candidate, ctx);
    attempt.elapsed_ms = current_ms() - started;

    if (result.ok()) {
      lease.value().finish(std::nullopt);
      attempt.ok = true;
      output.attempts.push_back(attempt);
      output.value = std::move(result).value();
      output.source = candidate;
      output.substituted = !(candidate == ref);
      return Result<AspectResult<T>>::success(std::move(output));
    }

    lease.value().finish(result.error());
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
//
// 同层内保持各库返回的自然顺序：候选库按优先级分层，层内按注册顺序
// 拼接，库内按来源自己的顺序。分页用「聚合 offset + 重放」：每次请求
// 从聚合流头部重新消费并丢掉 offset 条，服务端不保留会话。
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
  if (query.library_id.has_value()) {
    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
                       [&query](const LibraryRegistration &registration) {
                         return registration.library_id != *query.library_id;
                       }),
        candidates.end());
  }
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

  const std::uint64_t revision = search_revision(query, candidates);

  int skip = 0;
  if (query.page.cursor.has_value() && !query.page.cursor->empty()) {
    std::uint64_t cursor_revision = 0;
    int cursor_offset = 0;
    if (!decode_search_cursor(*query.page.cursor, cursor_offset,
                              cursor_revision) ||
        cursor_revision != revision) {
      return Result<AggregatedPage<SearchHit>>::failure(
          Error::make(ErrorCode::InvalidRequest, "invalid search cursor"));
    }
    skip = cursor_offset;
  } else {
    skip = std::clamp(query.page.offset, 0, kMaxSearchOffset);
  }

  const int limit = std::clamp(query.page.limit > 0 ? query.page.limit : 50,
                               1, kMaxSearchLimit);

  std::vector<SearchHit> items;
  items.reserve(static_cast<std::size_t>(limit));
  std::vector<Error> failures;
  std::map<std::string, SourceStatus> statuses;
  bool any_success = false;
  bool has_more = false;
  // 聚合流按 ref 去重：上游深 offset 会重复返回前面的结果（网易云搜索
  // 实测超过约 256 条后开始整页重复）。去重后的流才是稳定的聚合前缀。
  std::unordered_set<std::string> seen_refs;

  int remaining_skip = skip;
  std::size_t index = 0;
  while (index < candidates.size()) {
    const std::size_t tier = tier_of.at(candidates[index].library_id);
    std::size_t group_end = index + 1;
    while (group_end < candidates.size() &&
           tier_of.at(candidates[group_end].library_id) == tier) {
      ++group_end;
    }

    // 本层要凑出的前缀：跳过 remaining_skip、补齐本页，再多要一条用来
    // 判断后面还有没有结果。
    const int needed = remaining_skip +
                       (limit - static_cast<int>(items.size())) + 1;
    std::vector<SearchHit> tier_items;
    tier_items.reserve(static_cast<std::size_t>(needed));
    bool tier_has_more = false;

    for (std::size_t i = index; i < group_end; ++i) {
      const std::string &library_id = candidates[i].library_id;
      SourceStatus status;
      status.library_id = library_id;
      const std::int64_t started = current_ms();

      const int want = needed - static_cast<int>(tier_items.size());
      if (want <= 0) {
        status.state = SourceState::Skipped;
        statuses[library_id] = std::move(status);
        continue;
      }

      auto lease = registry_->acquire(library_id, CapabilityId::Search,
                                      now_ms(), ctx.deadline_ms);
      if (!lease.ok()) {
        status.state = source_state_from(lease.error().code);
        status.error = lease.error();
        status.elapsed_ms = current_ms() - started;
        statuses[library_id] = std::move(status);
        failures.push_back(lease.error());
        continue;
      }

      auto library = lease.value().library();
      auto searcher = library->capability<Searcher>();
      if (!searcher.has_value()) {
        Error error = Error::make(ErrorCode::Unsupported, "no searcher");
        error.library_id = library_id;
        lease.value().finish(error);
        status.state = source_state_from(error.code);
        status.error = error;
        status.elapsed_ms = current_ms() - started;
        statuses[library_id] = std::move(status);
        failures.push_back(std::move(error));
        continue;
      }

      int collected = 0;
      std::optional<std::string> source_cursor;
      bool source_ok = true;
      while (collected < want) {
        SearchQuery part = query;
        part.page.cursor = source_cursor;
        part.page.offset = 0;
        part.page.limit = kSourcePageSize;

        Result<Page<SearchHit>> result = searcher->get().search(part, ctx);
        if (!result.ok()) {
          source_ok = false;
          status.state = source_state_from(result.error().code);
          status.error = result.error();
          failures.push_back(result.error());
          break;
        }
        any_success = true;

        const std::size_t before = tier_items.size();
        for (SearchHit &hit : result.value().items) {
          std::string key = to_string(hit.ref);
          if (!seen_refs.insert(std::move(key)).second) {
            continue;
          }
          tier_items.push_back(std::move(hit));
        }
        const int added = static_cast<int>(tier_items.size() - before);
        collected += added;
        if (added == 0) {
          // 整页都是已出现过的结果：该来源没有更多新结果了。
          break;
        }

        if (result.value().items.empty()) {
          break;
        }
        if (collected >= want) {
          if (result.value().has_more ||
              result.value().next_cursor.has_value()) {
            tier_has_more = true;
          }
          break;
        }
        if (!result.value().has_more ||
            !result.value().next_cursor.has_value()) {
          break;
        }
        if (source_cursor.has_value() &&
            *result.value().next_cursor == *source_cursor) {
          break;
        }
        source_cursor = result.value().next_cursor;
      }

      status.item_count = collected;
      status.elapsed_ms = current_ms() - started;
      if (source_ok) {
        lease.value().finish(std::nullopt);
      } else {
        lease.value().finish(status.error);
      }
      statuses[library_id] = std::move(status);

      if (static_cast<int>(tier_items.size()) >= needed) {
        // 本层后面的库排在这个前缀之后，不需要查。
        for (std::size_t j = i + 1; j < group_end; ++j) {
          SourceStatus skipped;
          skipped.library_id = candidates[j].library_id;
          skipped.state = SourceState::Skipped;
          statuses[skipped.library_id] = std::move(skipped);
        }
        break;
      }
    }

    const int available = static_cast<int>(tier_items.size());
    if (remaining_skip >= available) {
      remaining_skip -= available;
      index = group_end;
      continue;
    }

    const int take = std::min(limit - static_cast<int>(items.size()),
                              available - remaining_skip);
    for (int i = 0; i < take; ++i) {
      items.push_back(std::move(
          tier_items[static_cast<std::size_t>(remaining_skip + i)]));
    }
    if (available > remaining_skip + take || tier_has_more) {
      has_more = true;
    }
    remaining_skip = 0;
    index = group_end;

    if (has_more) {
      break;
    }
  }

  // 全部来源失败且没有任何结果 -> 整体错误（5.3.10）。
  if (!any_success && items.empty() && !failures.empty()) {
    std::size_t chosen = 0;
    for (std::size_t i = 1; i < failures.size(); ++i) {
      if (aggregate_error_rank(failures[i].code) <
          aggregate_error_rank(failures[chosen].code)) {
        chosen = i;
      }
    }
    return Result<AggregatedPage<SearchHit>>::failure(failures[chosen]);
  }

  // 未查询的候选库也给出状态，保证 sources 覆盖全部候选。
  for (const LibraryRegistration &candidate : candidates) {
    if (statuses.count(candidate.library_id) == 0) {
      SourceStatus status;
      status.library_id = candidate.library_id;
      status.state = SourceState::Skipped;
      statuses.emplace(candidate.library_id, std::move(status));
    }
  }
  page.sources.reserve(candidates.size());
  for (const LibraryRegistration &candidate : candidates) {
    auto found = statuses.find(candidate.library_id);
    if (found != statuses.end()) {
      page.sources.push_back(std::move(found->second));
    }
  }

  page.items = std::move(items);
  page.has_more = has_more;
  if (has_more) {
    page.next_cursor = encode_search_cursor(
        skip + static_cast<int>(page.items.size()), revision);
  }
  return Result<AggregatedPage<SearchHit>>::success(std::move(page));
}

} // namespace media_library
