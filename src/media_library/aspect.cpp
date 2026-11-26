// Aspect 与能力、实体类型之间的纯映射。
//
// 从 entity_link.cpp 拆出来：这些函数不碰数据库，却一直和 EntityLink 的
// DB 实现绑在同一个编译单元里，导致任何只想用 Aspect 的调用方（例如
// Router 的单元测试）都得把 SOCI 一起拖进来。

#include "media_library/entity_link.h"

namespace media_library {

std::string_view aspect_name(Aspect aspect) noexcept {
  switch (aspect) {
  case Aspect::Metadata:
    return "metadata";
  case Aspect::Lyrics:
    return "lyrics";
  case Aspect::Stream:
    return "stream";
  case Aspect::Artwork:
    return "artwork";
  }
  return "metadata";
}

std::optional<Aspect> parse_aspect(std::string_view text) noexcept {
  if (text == "metadata") {
    return Aspect::Metadata;
  }
  if (text == "lyrics") {
    return Aspect::Lyrics;
  }
  if (text == "stream") {
    return Aspect::Stream;
  }
  if (text == "artwork") {
    return Aspect::Artwork;
  }
  return std::nullopt;
}

CapabilityId capability_for(Aspect aspect) noexcept {
  switch (aspect) {
  case Aspect::Metadata:
    return CapabilityId::CatalogRead;
  case Aspect::Lyrics:
    return CapabilityId::LyricsRead;
  case Aspect::Stream:
    return CapabilityId::StreamResolve;
  case Aspect::Artwork:
    return CapabilityId::ArtworkRead;
  }
  return CapabilityId::CatalogRead;
}

bool aspect_accepts(Aspect aspect, EntityType type) noexcept {
  switch (aspect) {
  case Aspect::Metadata:
    return type == EntityType::Track || type == EntityType::Album ||
           type == EntityType::Artist || type == EntityType::AlbumArtist ||
           type == EntityType::Playlist;
  case Aspect::Lyrics:
    return type == EntityType::Track;
  case Aspect::Stream:
    return type == EntityType::Track;
  case Aspect::Artwork:
    // 封面挂在任意 owner 上（曲目、专辑、艺术家、歌单）。
    return true;
  }
  return false;
}

} // namespace media_library