#include "media_library/media_router.h"

#include <chrono>
#include <utility>

namespace media_library {
namespace {

std::int64_t current_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 单源的一跳：库就是调用方指定的那个，chain 只有一跳。
std::vector<RouteAttempt> single_hop(const std::string &library_id,
                                     std::int64_t elapsed_ms) {
  RouteAttempt attempt;
  attempt.library_id = library_id;
  attempt.ok = true;
  attempt.elapsed_ms = elapsed_ms;
  return {std::move(attempt)};
}

// 单源信封：不换源、无方面、链只有一跳。
template <typename T>
Sourced<T> sourced_single(T value, const SourceRef &ref, std::int64_t elapsed_ms) {
  Sourced<T> output;
  output.origin.library_id = ref.library_id;
  output.origin.ref = ref;
  output.origin.aspect = std::nullopt;
  output.origin.substituted = false;
  output.origin.chain = single_hop(ref.library_id, elapsed_ms);
  output.value = std::move(value);
  return output;
}

// 多源信封：来源与替换事实都取自 5.3 的 AspectResult。
template <typename T>
Sourced<T> sourced_resolved(T value, const AspectResult<T> &resolved,
                            Aspect aspect) {
  Sourced<T> output;
  output.origin.library_id = resolved.source.library_id;
  output.origin.ref = resolved.source;
  output.origin.aspect = aspect;
  output.origin.substituted = resolved.substituted;
  output.origin.chain = to_route_chain(resolved.attempts);
  output.value = std::move(value);
  return output;
}

// 聚合的每一条：来源就是命中自带的库，不做单实体换源，链只有一跳。
template <typename T>
Sourced<T> sourced_aggregate(T value, const SourceRef &ref) {
  Sourced<T> output;
  output.origin.library_id = ref.library_id;
  output.origin.ref = ref;
  output.origin.aspect = std::nullopt;
  output.origin.substituted = false;
  output.origin.chain = single_hop(ref.library_id, 0);
  output.value = std::move(value);
  return output;
}

} // namespace

std::vector<RouteAttempt>
to_route_chain(const std::vector<AspectAttempt> &attempts) {
  std::vector<RouteAttempt> chain;
  chain.reserve(attempts.size());
  for (const AspectAttempt &attempt : attempts) {
    RouteAttempt hop;
    hop.library_id = attempt.library_id;
    hop.ok = attempt.ok;
    hop.error = attempt.error;
    hop.elapsed_ms = attempt.elapsed_ms;
    chain.push_back(std::move(hop));
  }
  return chain;
}

MediaRouterImpl::MediaRouterImpl(std::shared_ptr<Router> router)
    : router_(std::move(router)) {}

std::shared_ptr<MediaRouter> make_media_router(std::shared_ptr<Router> router) {
  return std::make_shared<MediaRouterImpl>(std::move(router));
}

// ---------------------------------------------------------------------------
// 5.4.3 单源读接口
// ---------------------------------------------------------------------------

Result<Sourced<Track>> MediaRouterImpl::get_track(const TrackRef &ref,
                                                  const CallContext &ctx) {
  const std::int64_t started = current_ms();
  Result<Track> result = router_->get_track(ref, ctx);
  if (!result.ok()) {
    return Result<Sourced<Track>>::failure(result.error());
  }
  return Result<Sourced<Track>>::success(
      sourced_single(std::move(result).value(), ref, current_ms() - started));
}

Result<Sourced<Album>> MediaRouterImpl::get_album(const AlbumRef &ref,
                                                  const CallContext &ctx) {
  const std::int64_t started = current_ms();
  Result<Album> result = router_->get_album(ref, ctx);
  if (!result.ok()) {
    return Result<Sourced<Album>>::failure(result.error());
  }
  return Result<Sourced<Album>>::success(
      sourced_single(std::move(result).value(), ref, current_ms() - started));
}

Result<Sourced<Artist>> MediaRouterImpl::get_artist(const ArtistRef &ref,
                                                    const CallContext &ctx) {
  const std::int64_t started = current_ms();
  Result<Artist> result = router_->get_artist(ref, ctx);
  if (!result.ok()) {
    return Result<Sourced<Artist>>::failure(result.error());
  }
  return Result<Sourced<Artist>>::success(
      sourced_single(std::move(result).value(), ref, current_ms() - started));
}

Result<Sourced<Playlist>>
MediaRouterImpl::get_playlist(const PlaylistRef &ref, const CallContext &ctx) {
  const std::int64_t started = current_ms();
  Result<Playlist> result = router_->get_playlist(ref, ctx);
  if (!result.ok()) {
    return Result<Sourced<Playlist>>::failure(result.error());
  }
  return Result<Sourced<Playlist>>::success(sourced_single(
      std::move(result).value(), ref, current_ms() - started));
}

Result<Sourced<Page<Track>>>
MediaRouterImpl::list_album_tracks(const AlbumRef &ref, const PageRequest &page,
                                   const CallContext &ctx) {
  const std::int64_t started = current_ms();
  Result<Page<Track>> result = router_->list_album_tracks(ref, page, ctx);
  if (!result.ok()) {
    return Result<Sourced<Page<Track>>>::failure(result.error());
  }
  return Result<Sourced<Page<Track>>>::success(sourced_single(
      std::move(result).value(), ref, current_ms() - started));
}

Result<Sourced<Page<Album>>>
MediaRouterImpl::list_artist_albums(const ArtistRef &ref,
                                    const PageRequest &page,
                                    const CallContext &ctx) {
  const std::int64_t started = current_ms();
  Result<Page<Album>> result = router_->list_artist_albums(ref, page, ctx);
  if (!result.ok()) {
    return Result<Sourced<Page<Album>>>::failure(result.error());
  }
  return Result<Sourced<Page<Album>>>::success(sourced_single(
      std::move(result).value(), ref, current_ms() - started));
}

Result<Sourced<Page<Playlist>>>
MediaRouterImpl::list_playlists(const std::string &library_id,
                                const PageRequest &page,
                                const CallContext &ctx) {
  const std::int64_t started = current_ms();
  Result<Page<Playlist>> result =
      router_->list_playlists(library_id, page, ctx);
  if (!result.ok()) {
    return Result<Sourced<Page<Playlist>>>::failure(result.error());
  }

  // 列表没有单一实体引用，origin.ref 表达“这个库”本身。
  SourceRef ref;
  ref.library_id = library_id;
  ref.entity_type = EntityType::Playlist;
  ref.entity_id.clear();

  return Result<Sourced<Page<Playlist>>>::success(
      sourced_single(std::move(result).value(), ref, current_ms() - started));
}

Result<Sourced<Page<SearchHit>>>
MediaRouterImpl::search_in(const std::string &library_id,
                           const SearchQuery &query, const CallContext &ctx) {
  const std::int64_t started = current_ms();
  Result<Page<SearchHit>> result = router_->search_in(library_id, query, ctx);
  if (!result.ok()) {
    return Result<Sourced<Page<SearchHit>>>::failure(result.error());
  }

  SourceRef ref;
  ref.library_id = library_id;
  ref.entity_type = EntityType::Track;
  ref.entity_id.clear();

  return Result<Sourced<Page<SearchHit>>>::success(sourced_single(
      std::move(result).value(), ref, current_ms() - started));
}

// ---------------------------------------------------------------------------
// 5.4.3 多源读接口
// ---------------------------------------------------------------------------

Result<Sourced<Track>>
MediaRouterImpl::resolve_track(const TrackRef &ref, const CallContext &ctx) {
  Result<AspectResult<Track>> result = router_->resolve_track(ref, ctx);
  if (!result.ok()) {
    return Result<Sourced<Track>>::failure(result.error());
  }
  AspectResult<Track> resolved = std::move(result).value();
  return Result<Sourced<Track>>::success(
      sourced_resolved(std::move(resolved.value), resolved, Aspect::Metadata));
}

Result<Sourced<Lyrics>>
MediaRouterImpl::resolve_lyrics(const TrackRef &ref, const CallContext &ctx) {
  Result<AspectResult<Lyrics>> result = router_->resolve_lyrics(ref, ctx);
  if (!result.ok()) {
    return Result<Sourced<Lyrics>>::failure(result.error());
  }
  AspectResult<Lyrics> resolved = std::move(result).value();
  return Result<Sourced<Lyrics>>::success(
      sourced_resolved(std::move(resolved.value), resolved, Aspect::Lyrics));
}

Result<Sourced<PlaybackSource>>
MediaRouterImpl::resolve_stream(const TrackRef &ref, const CallContext &ctx) {
  Result<AspectResult<PlaybackSource>> result =
      router_->resolve_playback(ref, ctx);
  if (!result.ok()) {
    return Result<Sourced<PlaybackSource>>::failure(result.error());
  }
  AspectResult<PlaybackSource> resolved = std::move(result).value();
  return Result<Sourced<PlaybackSource>>::success(
      sourced_resolved(std::move(resolved.value), resolved, Aspect::Stream));
}

Result<Sourced<ArtworkRef>>
MediaRouterImpl::resolve_artwork(const SourceRef &owner,
                                 const CallContext &ctx) {
  Result<AspectResult<ArtworkRef>> result = router_->resolve_artwork(owner, ctx);
  if (!result.ok()) {
    return Result<Sourced<ArtworkRef>>::failure(result.error());
  }
  AspectResult<ArtworkRef> resolved = std::move(result).value();
  return Result<Sourced<ArtworkRef>>::success(
      sourced_resolved(std::move(resolved.value), resolved, Aspect::Artwork));
}

// ---------------------------------------------------------------------------
// 5.4.3 聚合
// ---------------------------------------------------------------------------

Result<RoutedPage<SearchHit>>
MediaRouterImpl::search(const SearchQuery &query, const CallContext &ctx) {
  Result<AggregatedPage<SearchHit>> result = router_->search(query, ctx);
  if (!result.ok()) {
    return Result<RoutedPage<SearchHit>>::failure(result.error());
  }

  AggregatedPage<SearchHit> page = std::move(result).value();
  RoutedPage<SearchHit> output;
  output.next_cursor = std::move(page.next_cursor);
  output.has_more = page.has_more;
  output.sources = std::move(page.sources);

  output.items.reserve(page.items.size());
  for (SearchHit &hit : page.items) {
    const SourceRef ref = hit.ref;
    output.items.push_back(sourced_aggregate(std::move(hit), ref));
  }
  return Result<RoutedPage<SearchHit>>::success(std::move(output));
}

} // namespace media_library
