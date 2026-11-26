// 回归测试：网易云把数字字段返回成字符串时，客户端不得崩溃。
//
// 背景：field_int64() 的守卫放行 is_string，但 jsoncpp 的 asInt64() 遇到
// 字符串直接抛 "Value is not convertible to Int64."，异常穿透 parse_body
// 到 Drogon 顶层，getPlaylist 完全没有响应
// （netease-main:playlist:5246228300）。
//
// 这里用 FakeNeteaseTransport 注入平台原始 JSON，走完
// get_json -> parse_body -> parse_playlist/parse_song 的真实链路，
// 不访问网络、不读配置、不连库。

#include <gtest/gtest.h>

#include <cstdint>

#include "api/api-enhanced/client.h"
#include "api/api-enhanced/transport.h"

namespace {

using api_enhanced::FakeNeteaseTransport;
using api_enhanced::NeteaseClient;
using api_enhanced::NeteaseClientOptions;
using api_enhanced::NeteaseTransport;

// 复刻报障歌单的形状：数字字段全部被字符串化。
constexpr const char *kPlaylistDetailBody = R"json({
  "code": 200,
  "playlist": {
    "id": "5246228300",
    "name": "测试歌单",
    "coverImgUrl": "http://example.test/cover.jpg",
    "trackCount": "42",
    "updateTime": "1700000000000",
    "description": "描述",
    "tags": ["流行"],
    "creator": { "nickname": "作者" }
  }
})json";

// /playlist/track/all 的曲目形状，同样把数字写成字符串（cd 是最典型的一例）。
constexpr const char *kPlaylistTracksBody = R"json({
  "code": 200,
  "songs": [
    {
      "id": "186016",
      "name": "晴天",
      "dt": "269000",
      "fee": 0,
      "no": "1",
      "cd": "1",
      "publishTime": "1056931200000",
      "ar": [{ "id": "6452", "name": "周杰伦" }],
      "al": {
        "id": "18875",
        "name": "叶惠美",
        "picUrl": "http://example.test/al.jpg"
      }
    }
  ]
})json";

struct Fixture {
  std::shared_ptr<FakeNeteaseTransport> transport =
      std::make_shared<FakeNeteaseTransport>();
  NeteaseClient client;

  explicit Fixture(const char *path, const char *body)
      : client(transport, NeteaseClientOptions{}) {
    transport->set_reply(path, {true, body, media_library::ErrorCode::Internal});
  }
};

} // namespace

TEST(NeteaseNumericFieldTest, PlaylistDetailAcceptsStringifiedNumbers) {
  Fixture fixture("/playlist/detail", kPlaylistDetailBody);

  const media_library::Result<api_enhanced::NeteasePlaylist> result =
      fixture.client.playlist(5246228300, media_library::CallContext{});

  ASSERT_TRUE(result.ok()) << result.error().message;
  const api_enhanced::NeteasePlaylist &playlist = result.value();
  EXPECT_EQ(playlist.id, 5246228300);
  EXPECT_EQ(playlist.name, "测试歌单");
  EXPECT_EQ(playlist.cover_img_url, "http://example.test/cover.jpg");
  EXPECT_EQ(playlist.track_count, 42);
  EXPECT_EQ(playlist.update_time_ms, 1700000000000LL);
  EXPECT_EQ(playlist.creator_name, "作者");
  EXPECT_EQ(playlist.description, "描述");
  ASSERT_EQ(playlist.tags.size(), 1u);
  EXPECT_EQ(playlist.tags[0], "流行");
  EXPECT_EQ(fixture.transport->last_path(), "/playlist/detail");
}

TEST(NeteaseNumericFieldTest, PlaylistTracksAcceptsStringifiedNumbers) {
  Fixture fixture("/playlist/track/all", kPlaylistTracksBody);

  const media_library::Result<api_enhanced::NeteasePage<api_enhanced::NeteaseSong>>
      result = fixture.client.playlist_tracks(5246228300, 100, 0,
                                              media_library::CallContext{});

  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto &page = result.value();
  ASSERT_EQ(page.items.size(), 1u);

  const api_enhanced::NeteaseSong &song = page.items[0];
  EXPECT_EQ(song.id, 186016);
  EXPECT_EQ(song.name, "晴天");
  EXPECT_EQ(song.duration_ms, 269000);
  EXPECT_EQ(song.no, 1);
  EXPECT_EQ(song.cd, 1);
  EXPECT_EQ(song.publish_time_ms, 1056931200000LL);
  ASSERT_EQ(song.artists.size(), 1u);
  EXPECT_EQ(song.artists[0].id, 6452);
  EXPECT_EQ(song.album.id, 18875);
  EXPECT_EQ(fixture.transport->last_path(), "/playlist/track/all");
}