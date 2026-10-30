#ifndef MEDIA_LIBRARY_MEDIA_ROUTER_H
#define MEDIA_LIBRARY_MEDIA_ROUTER_H

#include "media_library/domain.h"
#include "media_library/entity_link.h"
#include "media_library/error.h"
#include "media_library/library.h"
#include "media_library/router.h"
#include "media_library/source_ref.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace media_library {

// ---------------------------------------------------------------------------
// 5.4.2 返回信封
//
// 领域模型（Track / Album / Lyrics / Page<T>）本身不含来源信息；来源是
// “本次请求由谁回答”这个事实，只存在于信封上。不要把 origin 塞进实体。
// ---------------------------------------------------------------------------

// 回退链上的一跳。字段与 5.3 的 AspectAttempt 一致，转换是一对一的。
struct RouteAttempt {
  std::string library_id;
  bool ok = false;                 // 这一跳是否成功；成功时不填 error
  std::optional<ErrorCode> error;   // 失败时必填，成功时为空
  std::int64_t elapsed_ms = 0;
};

// 本次执行的实际来源。与 value.ref 是两件事：
// value.ref 是实体身份（稳定），origin 是本次回答者（随优先级/回退变化）。
struct Origin {
  std::string library_id;           // 实际提供数据的库实例，必填
  SourceRef ref;                    // 实际提供数据的那条实体引用
  std::optional<Aspect> aspect;     // 多源路由为具体方面；单源/聚合为空
  bool substituted = false;         // 实际来源 != 调用方指定的来源
  std::vector<RouteAttempt> chain;  // 按尝试顺序；最后一跳即成功跳
};

// 所有跨层返回的统一包装。
template <typename T> struct Sourced {
  T value;        // 已由 Library 完成标准化（5.3.6），路由层不再改写
  Origin origin;
};

// 聚合返回：每条结果各自带来源，整体再带来源状态汇总。
template <typename T> struct RoutedPage {
  std::vector<Sourced<T>> items;
  std::optional<std::string> next_cursor;
  bool has_more = false;
  std::vector<SourceStatus> sources;  // 仅聚合模式非空，覆盖全部候选库
};

// 5.4.3 读接口。写接口属 5.4.5，本阶段不定义实现。
class MediaRouter {
public:
  virtual ~MediaRouter() = default;

  // —— 单源：ref.library_id 已确定，路由层不做任何选择（5.3.2）
  virtual Result<Sourced<Track>> get_track(const TrackRef &ref,
                                           const CallContext &ctx) = 0;
  virtual Result<Sourced<Album>> get_album(const AlbumRef &ref,
                                           const CallContext &ctx) = 0;
  virtual Result<Sourced<Artist>> get_artist(const ArtistRef &ref,
                                             const CallContext &ctx) = 0;
  virtual Result<Sourced<Playlist>> get_playlist(const PlaylistRef &ref,
                                                 const CallContext &ctx) = 0;

  virtual Result<Sourced<Page<Track>>>
  list_album_tracks(const AlbumRef &ref, const PageRequest &page,
                    const CallContext &ctx) = 0;
  virtual Result<Sourced<Page<Album>>>
  list_artist_albums(const ArtistRef &ref, const PageRequest &page,
                     const CallContext &ctx) = 0;
  virtual Result<Sourced<Page<Playlist>>>
  list_playlists(const std::string &library_id, const PageRequest &page,
                 const CallContext &ctx) = 0;

  // 单源搜索：库由调用方显式指定；命中 0 条就是 0 条，不从其他库补。
  virtual Result<Sourced<Page<SearchHit>>>
  search_in(const std::string &library_id, const SearchQuery &query,
            const CallContext &ctx) = 0;

  // —— 多源：只给 ref，按方面优先级选源（5.3.3）；查不到 link 退化为单源
  virtual Result<Sourced<Track>> resolve_track(const TrackRef &ref,
                                               const CallContext &ctx) = 0;
  virtual Result<Sourced<Lyrics>> resolve_lyrics(const TrackRef &ref,
                                                 const CallContext &ctx) = 0;
  virtual Result<Sourced<PlaybackSource>>
  resolve_stream(const TrackRef &ref, const CallContext &ctx) = 0;
  virtual Result<Sourced<ArtworkRef>> resolve_artwork(const SourceRef &owner,
                                                      const CallContext &ctx) = 0;

  // —— 聚合：查询条件，跨来源合并（5.3.4）
  // 不带关键词、只用筛选条件的“浏览”是它的参数形态，不另设方法。
  virtual Result<RoutedPage<SearchHit>> search(const SearchQuery &query,
                                               const CallContext &ctx) = 0;
};

// 门面实现：把 5.3 Router 的裸领域模型结果包成信封。
// 自己不做选源、不回退、不排序——那些只在 Router 里做一份。
class MediaRouterImpl final : public MediaRouter {
public:
  explicit MediaRouterImpl(std::shared_ptr<Router> router);

  Result<Sourced<Track>> get_track(const TrackRef &ref,
                                   const CallContext &ctx) override;
  Result<Sourced<Album>> get_album(const AlbumRef &ref,
                                   const CallContext &ctx) override;
  Result<Sourced<Artist>> get_artist(const ArtistRef &ref,
                                     const CallContext &ctx) override;
  Result<Sourced<Playlist>> get_playlist(const PlaylistRef &ref,
                                        const CallContext &ctx) override;

  Result<Sourced<Page<Track>>>
  list_album_tracks(const AlbumRef &ref, const PageRequest &page,
                    const CallContext &ctx) override;
  Result<Sourced<Page<Album>>>
  list_artist_albums(const ArtistRef &ref, const PageRequest &page,
                     const CallContext &ctx) override;
  Result<Sourced<Page<Playlist>>>
  list_playlists(const std::string &library_id, const PageRequest &page,
                 const CallContext &ctx) override;

  Result<Sourced<Page<SearchHit>>>
  search_in(const std::string &library_id, const SearchQuery &query,
            const CallContext &ctx) override;

  Result<Sourced<Track>> resolve_track(const TrackRef &ref,
                                       const CallContext &ctx) override;
  Result<Sourced<Lyrics>> resolve_lyrics(const TrackRef &ref,
                                         const CallContext &ctx) override;
  Result<Sourced<PlaybackSource>>
  resolve_stream(const TrackRef &ref, const CallContext &ctx) override;
  Result<Sourced<ArtworkRef>> resolve_artwork(const SourceRef &owner,
                                              const CallContext &ctx) override;

  Result<RoutedPage<SearchHit>> search(const SearchQuery &query,
                                       const CallContext &ctx) override;

private:
  std::shared_ptr<Router> router_;
};

std::shared_ptr<MediaRouter> make_media_router(std::shared_ptr<Router> router);

// 把 5.3 的回退链转成 5.4 的 RouteAttempt 链。
std::vector<RouteAttempt>
to_route_chain(const std::vector<AspectAttempt> &attempts);

} // namespace media_library

#endif // MEDIA_LIBRARY_MEDIA_ROUTER_H
