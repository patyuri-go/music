// SourceRef 编解码单元测试。
//
// 覆盖：五种实体类型的名称往返、简单格式、全类型往返、entity_id 转义、
// 小写十六进制解码、非法输入，以及曲目/专辑艺术家引用的显式转换。

#include "media_library/source_ref.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using media_library::EntityType;
using media_library::SourceRef;

void expectRef(const std::optional<SourceRef> &ref, std::string_view library_id,
               EntityType type, std::string_view entity_id) {
  ASSERT_TRUE(ref.has_value());
  EXPECT_EQ(ref->library_id, std::string(library_id));
  EXPECT_EQ(ref->entity_type, type);
  EXPECT_EQ(ref->entity_id, std::string(entity_id));
}

} // namespace

TEST(SourceRefTest, EntityTypeNamesRoundTrip) {
  const std::vector<std::pair<EntityType, std::string>> cases = {
      {EntityType::Track, "track"},
      {EntityType::Album, "album"},
      {EntityType::Artist, "artist"},
      {EntityType::AlbumArtist, "album_artist"},
      {EntityType::Playlist, "playlist"},
  };
  for (const auto &[type, name] : cases) {
    SCOPED_TRACE(name);
    EXPECT_EQ(std::string(media_library::entity_type_name(type)), name);
    const auto parsed = media_library::parse_entity_type(name);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, type);
  }

  EXPECT_FALSE(media_library::parse_entity_type("").has_value());
  EXPECT_FALSE(media_library::parse_entity_type("Track").has_value());
  EXPECT_FALSE(media_library::parse_entity_type("bogus").has_value());
}

TEST(SourceRefTest, FormatsSimpleRef) {
  const SourceRef ref{"local-main", EntityType::Track, "123"};
  EXPECT_EQ(media_library::to_string(ref), "local-main:track:123");
}

TEST(SourceRefTest, RoundTripsAllEntityTypes) {
  const std::vector<EntityType> types = {
      EntityType::Track,       EntityType::Album,      EntityType::Artist,
      EntityType::AlbumArtist, EntityType::Playlist,
  };
  for (const EntityType type : types) {
    SCOPED_TRACE(static_cast<int>(type));
    const SourceRef ref{"local-main", type, "42"};
    const auto parsed = media_library::parse_ref(media_library::to_string(ref));
    expectRef(parsed, "local-main", type, "42");
  }
}

TEST(SourceRefTest, EncodesSpecialCharactersInEntityId) {
  const SourceRef ref{"local-main", EntityType::Track, "a:b c/中%"};
  EXPECT_EQ(media_library::to_string(ref),
            "local-main:track:a%3Ab%20c%2F%E4%B8%AD%25");

  const auto parsed = media_library::parse_ref(media_library::to_string(ref));
  expectRef(parsed, "local-main", EntityType::Track, "a:b c/中%");
}

TEST(SourceRefTest, DecodesLowercaseHex) {
  const auto parsed =
      media_library::parse_ref("local-main:track:a%3ab%2fc");
  expectRef(parsed, "local-main", EntityType::Track, "a:b/c");
}

TEST(SourceRefTest, RejectsMalformedRefs) {
  const std::vector<std::string> cases = {
      "",
      "local-main",
      "local-main:track",
      ":track:1",
      "local-main:track:",
      "local-main:bogus:1",
      "local-main:track:%",
      "local-main:track:%2",
      "local-main:track:%ZZ",
  };
  for (const std::string &text : cases) {
    SCOPED_TRACE(text);
    EXPECT_FALSE(media_library::parse_ref(text).has_value());
  }
}

TEST(SourceRefTest, ConvertsBetweenArtistRefKinds) {
  const media_library::AlbumArtistRef album_artist{
      "local-main", EntityType::AlbumArtist, "7"};
  const media_library::ArtistRef artist =
      media_library::as_artist_ref(album_artist);
  EXPECT_EQ(artist.library_id, "local-main");
  EXPECT_EQ(artist.entity_type, EntityType::Artist);
  EXPECT_EQ(artist.entity_id, "7");

  const media_library::AlbumArtistRef back =
      media_library::as_album_artist_ref(artist);
  EXPECT_EQ(back.library_id, "local-main");
  EXPECT_EQ(back.entity_type, EntityType::AlbumArtist);
  EXPECT_EQ(back.entity_id, "7");
}