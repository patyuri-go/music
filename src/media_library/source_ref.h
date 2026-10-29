#ifndef MEDIA_LIBRARY_SOURCE_REF_H
#define MEDIA_LIBRARY_SOURCE_REF_H

#include <optional>
#include <string>
#include <string_view>

namespace media_library {

enum class EntityType { Track, Album, Artist, AlbumArtist, Playlist };

struct SourceRef {
  std::string library_id;
  EntityType entity_type = EntityType::Track;
  std::string entity_id;

  bool operator==(const SourceRef &other) const noexcept {
    return library_id == other.library_id &&
           entity_type == other.entity_type && entity_id == other.entity_id;
  }
};

using TrackRef = SourceRef;
using AlbumRef = SourceRef;
using ArtistRef = SourceRef;
using AlbumArtistRef = SourceRef;
using PlaylistRef = SourceRef;

std::string_view entity_type_name(EntityType type) noexcept;
std::optional<EntityType> parse_entity_type(std::string_view text) noexcept;

std::string to_string(const SourceRef &ref);
std::optional<SourceRef> parse_ref(std::string_view text);

// 同一个底层艺术家可以作为曲目艺术家或专辑艺术家被引用。
// 转换显式进行，避免两种引用被隐式混用。
ArtistRef as_artist_ref(const AlbumArtistRef &ref);
AlbumArtistRef as_album_artist_ref(const ArtistRef &ref);

} // namespace media_library

#endif // MEDIA_LIBRARY_SOURCE_REF_H
