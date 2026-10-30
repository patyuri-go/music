#ifndef MEDIA_LIBRARY_ROUTER_H
#define MEDIA_LIBRARY_ROUTER_H

#include "media_library/domain.h"
#include "media_library/entity_link.h"
#include "media_library/error.h"
#include "media_library/library.h"
#include "media_library/registry.h"
#include "media_library/source_ref.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace media_library {

// 5.3.5 来源状态。
enum class SourceState {
  Ok,
  Partial,
  Unavailable,
  Unauthenticated,
  RateLimited,
  Timeout,
  Unsupported
};

std::string_view source_state_name(SourceState state) noexcept;
SourceState source_state_from(ErrorCode code) noexcept;

struct SourceStatus {
  std::string library_id;
  SourceState state = SourceState::Ok;
  std::optional<Error> error;
  int item_count = 0;
  std::int64_t elapsed_ms = 0;
};

// 5.3.9 聚合优先级：多个库的结果集怎么分层、怎么排、怎么消费。
// 与 5.3.3 的方面优先级是两套独立配置，不要混用。
class AggregatePriority {
public:
  virtual ~AggregatePriority() = default;

  virtual std::vector<std::string> order() const = 0;
  virtual Status set_order(std::vector<std::string> library_ids) = 0;
  virtual std::uint64_t revision() const = 0;
};

std::shared_ptr<AggregatePriority> make_memory_aggregate_priority();

// 一次多源尝试的记录（5.3.3「记录来源与回退原因」）。
struct AspectAttempt {
  std::string library_id;
  bool ok = false;
  std::optional<ErrorCode> error;
  std::int64_t elapsed_ms = 0;
};

// 多源路由的结果。source 是实际提供数据的库，substituted 表示
// 实际来源与调用方给出的 SourceRef 不同（即发生了换源）。
// 对外形态（Sourced<T> / Origin）见 5.4，本节只到这一层。
template <typename T> struct AspectResult {
  T value{};
  SourceRef source;
  bool substituted = false;
  std::vector<AspectAttempt> attempts;
};

template <typename T> struct AggregatedPage {
  std::vector<T> items;
  std::optional<std::string> next_cursor;
  bool has_more = false;
  std::vector<SourceStatus> sources;
};

class AspectRouter {
public:
  virtual ~AspectRouter() = default;

  // 只选 SourceRef，不取数据（5.3.12）。
  virtual Result<SourceRef> resolve(const SourceRef &ref, Aspect aspect,
                                    const CallContext &ctx) const = 0;
};

class AggregateRouter {
public:
  virtual ~AggregateRouter() = default;

  virtual Result<AggregatedPage<SearchHit>>
  search(const SearchQuery &query, const CallContext &ctx) = 0;
};

// 路由层门面。同时承担 5.3.2 单源、5.3.3 多源、5.3.4 聚合三种模式。
class Router final : public AspectRouter, public AggregateRouter {
public:
  Router(std::shared_ptr<LibraryRegistry> registry,
         std::shared_ptr<EntityLinkResolver> links,
         std::shared_ptr<AspectPriority> aspect_priority,
         std::shared_ptr<AggregatePriority> aggregate_priority);
  ~Router() override = default;

  // ---- 5.3.2 单源路由 ---------------------------------------------------
  // 调用方已指定 library_id；不做任何选择、不读任何优先级、不回退。
  Result<Track> get_track(const TrackRef &ref, const CallContext &ctx);
  Result<Album> get_album(const AlbumRef &ref, const CallContext &ctx);
  Result<Artist> get_artist(const ArtistRef &ref, const CallContext &ctx);
  Result<Playlist> get_playlist(const PlaylistRef &ref,
                                const CallContext &ctx);
  Result<Page<Track>> list_album_tracks(const AlbumRef &ref,
                                        const PageRequest &page,
                                        const CallContext &ctx);
  Result<Page<Album>> list_artist_albums(const ArtistRef &ref,
                                         const PageRequest &page,
                                         const CallContext &ctx);
  // 库内歌单列表：库由 library_id 显式指定，不做任何选择。
  Result<Page<Playlist>> list_playlists(const std::string &library_id,
                                        const PageRequest &page,
                                        const CallContext &ctx);

  // 单源搜索：命中 0 条就是 0 条，绝不从其他库补（5.3.2）。
  Result<Page<SearchHit>> search_in(const std::string &library_id,
                                    const SearchQuery &query,
                                    const CallContext &ctx);

  // ---- 5.3.3 多源路由 ---------------------------------------------------
  Result<SourceRef> resolve(const SourceRef &ref, Aspect aspect,
                            const CallContext &ctx) const override;

  // 多源执行：按方面优先级选源、执行、失败按同一顺序回退。
  Result<AspectResult<Track>> resolve_track(const TrackRef &ref,
                                            const CallContext &ctx);
  Result<AspectResult<Lyrics>> resolve_lyrics(const TrackRef &ref,
                                              const CallContext &ctx);
  Result<AspectResult<PlaybackSource>>
  resolve_playback(const TrackRef &ref, const CallContext &ctx);
  Result<AspectResult<ArtworkRef>> resolve_artwork(const SourceRef &owner,
                                                   const CallContext &ctx);

  // ---- 5.3.4 多源聚合 ---------------------------------------------------
  // 本轮只做第一页：next_cursor 恒为空，has_more 表示来源还有更多。
  Result<AggregatedPage<SearchHit>> search(const SearchQuery &query,
                                           const CallContext &ctx) override;

  // 返回该方面在该链接下的最终有序库列表（诊断与测试用）。
  std::vector<std::string> order_of(const SourceRef &ref,
                                    Aspect aspect) const;

private:
  // 取库并占用一个并发名额（5.2.6 的 admit）。
  // 成功后调用方必须配对 release：用文件内的 AdmissionGuard 保证。
  Result<std::shared_ptr<Library>> acquire(const std::string &library_id,
                                           CapabilityId capability) const;

  // 多源候选：硬过滤 + 按方面优先级排序。空表示没有可用候选。
  // 返回完整 SourceRef，因为选定库之后还要用该库自己的引用去取数据。
  std::vector<SourceRef> candidates_for(const SourceRef &ref, Aspect aspect,
                                        const CallContext &ctx) const;

  // 单次多源执行：按顺序尝试，按回退规则决定是否继续。
  template <typename T, typename Executor>
  Result<AspectResult<T>> run_aspect(const SourceRef &ref, Aspect aspect,
                                     const CallContext &ctx,
                                     Executor &&executor);

  std::int64_t now_ms() const;
  static bool should_fall_back(ErrorCode code) noexcept;

  std::shared_ptr<LibraryRegistry> registry_;
  std::shared_ptr<EntityLinkResolver> links_;
  std::shared_ptr<AspectPriority> aspect_priority_;
  std::shared_ptr<AggregatePriority> aggregate_priority_;
};

std::shared_ptr<Router>
make_router(std::shared_ptr<LibraryRegistry> registry,
            std::shared_ptr<EntityLinkResolver> links,
            std::shared_ptr<AspectPriority> aspect_priority,
            std::shared_ptr<AggregatePriority> aggregate_priority);

// 内建兜底顺序（三级回落的第 3 级）：本地库优先，其余按注册项 priority。
std::shared_ptr<BuiltinOrderSource>
make_registry_builtin_order(std::shared_ptr<LibraryRegistry> registry);

} // namespace media_library

#endif // MEDIA_LIBRARY_ROUTER_H
