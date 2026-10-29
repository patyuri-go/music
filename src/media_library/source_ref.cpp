#include "media_library/source_ref.h"

#include <cctype>
#include <vector>

namespace media_library {
namespace {

constexpr char kHex[] = "0123456789ABCDEF";

int hexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

bool isUnreserved(unsigned char c) {
  return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~';
}

std::string encode(const std::string &text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    if (isUnreserved(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

std::optional<std::string> decode(const std::string &text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      out.push_back(text[i]);
      continue;
    }
    if (i + 2 >= text.size()) {
      return std::nullopt;
    }
    const int hi = hexValue(text[i + 1]);
    const int lo = hexValue(text[i + 2]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out.push_back(static_cast<char>((hi << 4) | lo));
    i += 2;
  }
  return out;
}

} // namespace

std::string_view entity_type_name(EntityType type) noexcept {
  switch (type) {
  case EntityType::Track:
    return "track";
  case EntityType::Album:
    return "album";
  case EntityType::Artist:
    return "artist";
  case EntityType::AlbumArtist:
    return "album_artist";
  case EntityType::Playlist:
    return "playlist";
  }
  return "";
}

std::optional<EntityType> parse_entity_type(std::string_view text) noexcept {
  if (text == "track") {
    return EntityType::Track;
  }
  if (text == "album") {
    return EntityType::Album;
  }
  if (text == "artist") {
    return EntityType::Artist;
  }
  if (text == "album_artist") {
    return EntityType::AlbumArtist;
  }
  if (text == "playlist") {
    return EntityType::Playlist;
  }
  return std::nullopt;
}

std::string to_string(const SourceRef &ref) {
  return ref.library_id + ":" + std::string(entity_type_name(ref.entity_type)) +
         ":" + encode(ref.entity_id);
}

std::optional<SourceRef> parse_ref(std::string_view text) {
  const std::size_t first = text.find(':');
  if (first == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t second = text.find(':', first + 1);
  if (second == std::string_view::npos) {
    return std::nullopt;
  }
  SourceRef ref;
  ref.library_id = std::string(text.substr(0, first));
  const auto type = parse_entity_type(text.substr(first + 1, second - first - 1));
  if (!type) {
    return std::nullopt;
  }
  ref.entity_type = *type;
  const auto decoded = decode(std::string(text.substr(second + 1)));
  if (!decoded || decoded->empty() || ref.library_id.empty()) {
    return std::nullopt;
  }
  ref.entity_id = *decoded;
  return ref;
}

ArtistRef as_artist_ref(const AlbumArtistRef &ref) {
  ArtistRef out = ref;
  out.entity_type = EntityType::Artist;
  return out;
}

AlbumArtistRef as_album_artist_ref(const ArtistRef &ref) {
  AlbumArtistRef out = ref;
  out.entity_type = EntityType::AlbumArtist;
  return out;
}

} // namespace media_library
