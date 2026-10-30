#ifndef MEDIA_LIBRARY_LIBRARY_H
#define MEDIA_LIBRARY_LIBRARY_H

#include "media_library/domain.h"
#include "media_library/error.h"
#include "media_library/source_ref.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace media_library {

struct CallContext {
  std::optional<std::int64_t> deadline_ms;
  std::string locale;
};

struct LibraryIdentity {
  std::string library_id;
  std::string display_name;
  std::string platform;
};

enum class CapabilityId {
  CatalogRead,
  Search,
  StreamResolve,
  LyricsRead,
  ArtworkRead,
  PlaylistRead,
  PlaylistWrite,
  FavoriteRead,
  FavoriteWrite,
  LibraryWrite,
  Auth
};

// 能力名。只在日志、指标与错误边界使用；内部判断一律用枚举。
constexpr std::string_view capability_name(CapabilityId id) noexcept {
  switch (id) {
  case CapabilityId::CatalogRead:
    return "catalog_read";
  case CapabilityId::Search:
    return "search";
  case CapabilityId::StreamResolve:
    return "stream_resolve";
  case CapabilityId::LyricsRead:
    return "lyrics_read";
  case CapabilityId::ArtworkRead:
    return "artwork_read";
  case CapabilityId::PlaylistRead:
    return "playlist_read";
  case CapabilityId::PlaylistWrite:
    return "playlist_write";
  case CapabilityId::FavoriteRead:
    return "favorite_read";
  case CapabilityId::FavoriteWrite:
    return "favorite_write";
  case CapabilityId::LibraryWrite:
    return "library_write";
  case CapabilityId::Auth:
    return "auth";
  }
  return "unknown";
}

struct CapabilityState {
  CapabilityId id = CapabilityId::CatalogRead;
  bool supported = false;
  bool available = false;
  std::optional<ErrorCode> reason;
  std::string detail;
};

struct CapabilitySet {
  std::vector<CapabilityState> items;

  std::optional<std::reference_wrapper<const CapabilityState>>
  find(CapabilityId id) const noexcept {
    for (const auto &item : items) {
      if (item.id == id) {
        return std::cref(item);
      }
    }
    return std::nullopt;
  }

  bool supports(CapabilityId id) const noexcept {
    const auto item = find(id);
    return item.has_value() && item->get().supported;
  }

  bool available(CapabilityId id) const noexcept {
    const auto item = find(id);
    return item.has_value() && item->get().supported && item->get().available;
  }
};

class CatalogReader {
public:
  virtual ~CatalogReader() = default;
  virtual Result<Track> get_track(const TrackRef &ref,
                                  const CallContext &ctx) = 0;
  virtual Result<Album> get_album(const AlbumRef &ref,
                                  const CallContext &ctx) = 0;
  virtual Result<Artist> get_artist(const ArtistRef &ref,
                                    const CallContext &ctx) = 0;
  virtual Result<Page<Track>> list_album_tracks(const AlbumRef &ref,
                                                const PageRequest &page,
                                                const CallContext &ctx) = 0;
  virtual Result<Page<Album>> list_artist_albums(const ArtistRef &ref,
                                                 const PageRequest &page,
                                                 const CallContext &ctx) = 0;
};

class Searcher {
public:
  virtual ~Searcher() = default;
  virtual Result<Page<SearchHit>> search(const SearchQuery &query,
                                         const CallContext &ctx) = 0;
};

class StreamResolver {
public:
  virtual ~StreamResolver() = default;
  virtual Result<PlaybackSource> resolve_playback(const TrackRef &ref,
                                                  const CallContext &ctx) = 0;
};

class LyricsProvider {
public:
  virtual ~LyricsProvider() = default;
  virtual Result<Lyrics> get_lyrics(const TrackRef &ref,
                                    const CallContext &ctx) = 0;
};

class ArtworkProvider {
public:
  virtual ~ArtworkProvider() = default;
  virtual Result<ArtworkRef> get_artwork(const SourceRef &owner,
                                         const CallContext &ctx) = 0;
};

class PlaylistReader {
public:
  virtual ~PlaylistReader() = default;
  virtual Result<Page<Playlist>> list_playlists(const PageRequest &page,
                                                const CallContext &ctx) = 0;
  virtual Result<Playlist> get_playlist(const PlaylistRef &ref,
                                        const CallContext &ctx) = 0;
};

class Library {
public:
  virtual ~Library() = default;

  virtual LibraryIdentity identity() const = 0;
  virtual CapabilitySet capabilities() const = 0;
  virtual void close() noexcept = 0;

  template <typename T>
  std::optional<std::reference_wrapper<T>> capability() noexcept {
    return lookup_capability<T>();
  }

  template <typename T>
  std::optional<std::reference_wrapper<const T>> capability() const noexcept {
    return lookup_capability<const T>();
  }

protected:
  template <typename T> void register_capability(std::shared_ptr<T> capability) {
    capabilities_[std::type_index(typeid(T))] = std::move(capability);
  }

private:
  template <typename T>
  std::optional<std::reference_wrapper<T>> lookup_capability() const noexcept {
    const auto it = capabilities_.find(std::type_index(typeid(T)));
    if (it == capabilities_.end()) {
      return std::nullopt;
    }
    return std::ref(*std::static_pointer_cast<T>(it->second));
  }

  std::unordered_map<std::type_index, std::shared_ptr<void>> capabilities_;
};

} // namespace media_library

#endif // MEDIA_LIBRARY_LIBRARY_H
