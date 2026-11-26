#ifndef MUSICD_TEST_FIXTURE_IDS_H
#define MUSICD_TEST_FIXTURE_IDS_H

// 集成测试夹具的固定常量。
//
// test_fixture 用这些值写库；fake_storage 按实体 UUID 提供字节；
// 各测试程序通过 test_fixture 输出的环境变量拿到真实行 id。
// 所有名称都带 __fixture__ 前缀，清理时按前缀删除，不会碰到生产数据。

#include <cstdint>
#include <string>
#include <vector>

namespace fixture {

inline const std::string kPrefix = "__fixture__";
inline const std::string kRootPath = "/tmp/opencode/musicd-fixture-root";

// ---- 专辑 ----
inline const std::string kAlbumName = "__fixture__ Album One";
inline const std::string kAlbumArtistKey = "__fixture__albumone";
inline const std::string kAlbumEntity = "aaaaaaaa-0000-4000-8000-000000000001";
inline const std::string kAlbumCoverPath =
    "/tmp/opencode/musicd-fixture-root/cover.jpg";

// 封面实体为 NULL 的专辑：封面请求必须 404，而不是回退到路径。
inline const std::string kAlbumNullName = "__fixture__ Album No Entity";
inline const std::string kAlbumNullArtistKey = "__fixture__albumnull";
inline const std::string kAlbumNullEntity; // empty => NULL

// ---- 艺术家 ----
// 第 1 首曲目的角色串会被扫描器拆成 4 个名字；现在直接种 track_artist。
inline const std::vector<std::string> kRoleArtists = {
    "__fixture__ typeMARS", "__fixture__ GhostFinal",
    "__fixture__ 瑶山百霊", "__fixture__ TurtleIzzy"};
inline const std::vector<std::string> kPlainArtists = {
    "__fixture__ Artist Alpha", "__fixture__ Artist Beta"};

// ---- 曲目 ----
inline const std::string kTrackRoleTitle = "__fixture__ Track Role";
inline const std::string kTrackRolePath =
    "/tmp/opencode/musicd-fixture-root/track-role.wav";
inline const std::string kTrackRoleEntity = "11111111-0000-4000-8000-000000000001";

inline const std::string kTrackPlainTitle = "__fixture__ Track Plain";
inline const std::string kTrackPlainPath =
    "/tmp/opencode/musicd-fixture-root/track-plain.wav";
inline const std::string kTrackPlainEntity =
    "22222222-0000-4000-8000-000000000002";

// 本地没有实体、但连到网易云的多来源链接上（跨库取流的主用例）。
inline const std::string kTrackNullTitle = "__fixture__ Track No Entity";
inline const std::string kTrackNullPath =
    "/tmp/opencode/musicd-fixture-root/track-null.wav";
inline const std::string kTrackNullEntity; // empty => NULL

// 本地没有实体、也没有任何链接的曲目：流请求必须 404。
inline const std::string kTrackUnlinkedNullTitle =
    "__fixture__ Track Unlinked Null";
inline const std::string kTrackUnlinkedNullPath =
    "/tmp/opencode/musicd-fixture-root/track-unlinked-null.wav";
inline const std::string kTrackUnlinkedNullEntity; // empty => NULL

// 属于「无封面实体」专辑的曲目，用于专辑封面 404 场景。
inline const std::string kTrackAlbumNullTitle = "__fixture__ Track Album Null";
inline const std::string kTrackAlbumNullPath =
    "/tmp/opencode/musicd-fixture-root/track-album-null.wav";
inline const std::string kTrackAlbumNullEntity =
    "44444444-0000-4000-8000-000000000004";

inline constexpr std::int64_t kDurationMs = 210000;
inline constexpr std::int64_t kSizeBytes = 1048576;
inline const std::string kFormat = "wav";

// 内嵌同步歌词：Jellyfin 需要 Start=15000000。
inline const std::string kSyncedLyrics = "[00:01.50]第一行\n[00:02.50]第二行\n";

// ---- 假 PG 夹具的固定内部 id ----
// fake_pg 按这些 id 返回数据；test_fixture env 按同样的值导出环境变量，
// 两者必须保持一致，否则集成测试取到的 id 与假库里的行对不上。
inline constexpr std::int64_t kFixedRootId = 1;
inline constexpr std::int64_t kFixedAlbumId = 2;
inline constexpr std::int64_t kFixedAlbumNullId = 3;
inline constexpr std::int64_t kFixedRoleArtistBaseId = 4;  // 4,5,6,7
inline constexpr std::int64_t kFixedPlainArtistBaseId = 8; // 8,9
inline constexpr std::int64_t kFixedTrackRoleId = 10;
inline constexpr std::int64_t kFixedTrackPlainId = 11;
inline constexpr std::int64_t kFixedTrackNullId = 12;
inline constexpr std::int64_t kFixedTrackAlbumNullId = 13;
inline constexpr std::int64_t kFixedTrackUnlinkedNullId = 14;

// ---- test_fixture 输出的环境变量名 ----
inline const char *kEnvRootPath = "FIXTURE_ROOT_PATH";
inline const char *kEnvAlbumId = "FIXTURE_ALBUM_ID";
inline const char *kEnvAlbumRef = "FIXTURE_ALBUM_REF";
inline const char *kEnvAlbumEntity = "FIXTURE_ALBUM_ENTITY";
inline const char *kEnvAlbumNullId = "FIXTURE_ALBUM_NULL_ID";
inline const char *kEnvAlbumNullRef = "FIXTURE_ALBUM_NULL_REF";
inline const char *kEnvTrackRoleId = "FIXTURE_TRACK_ROLE_ID";
inline const char *kEnvTrackRoleRef = "FIXTURE_TRACK_ROLE_REF";
inline const char *kEnvTrackRoleEntity = "FIXTURE_TRACK_ROLE_ENTITY";
inline const char *kEnvTrackPlainId = "FIXTURE_TRACK_PLAIN_ID";
inline const char *kEnvTrackPlainRef = "FIXTURE_TRACK_PLAIN_REF";
inline const char *kEnvTrackPlainEntity = "FIXTURE_TRACK_PLAIN_ENTITY";
inline const char *kEnvTrackNullId = "FIXTURE_TRACK_NULL_ID";
inline const char *kEnvTrackNullRef = "FIXTURE_TRACK_NULL_REF";
inline const char *kEnvTrackUnlinkedNullId = "FIXTURE_TRACK_UNLINKED_NULL_ID";
inline const char *kEnvTrackUnlinkedNullRef = "FIXTURE_TRACK_UNLINKED_NULL_REF";

// ---- 假网易云 API 的固定 id ----
// fake_netease 与 cross_library_test 共用，避免两侧对不上。
inline const std::string kNeteaseLibraryId = "netease-main";
inline const std::string kNeteaseSongId = "186016";        // 正常可播放（12 号的链接）
inline const std::string kNeteaseBlockedSongId = "186017"; // fee == 4，版权不可用
// 10 号（本地有实体）那条链接指向的 id：用来验证「本地解析成功但存储读不到
// 字节」时会不会换源。长度与另外两份都不同，测试据此判断拿到的是哪一份。
inline const std::string kNeteaseEntitySongId = "186018";

// 假网易云返回的音频字节：长度刻意避开本地 wav 的 1 MiB，
// 前 4 字节是 magic，其余按固定公式填充，测试端用同一个函数核对。
inline std::string neteaseAudioBytes(std::int64_t size, unsigned seed,
                                    const char *magic) {
  std::string out(static_cast<std::size_t>(size), '\0');
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<char>((i * 31 + seed * 17 + 7) % 251);
  }
  for (std::size_t i = 0; i < 4 && i < out.size(); ++i) {
    out[i] = magic[i];
  }
  return out;
}

inline constexpr std::int64_t kNeteaseAudioSize = 131072;
inline std::string kNeteaseAudioBytes() {
  return neteaseAudioBytes(kNeteaseAudioSize, 5, "FNE1");
}

inline constexpr std::int64_t kNeteaseEntityAudioSize = 131079;
inline std::string kNeteaseEntityAudioBytes() {
  return neteaseAudioBytes(kNeteaseEntityAudioSize, 6, "FNE2");
}

} // namespace fixture

#endif // MUSICD_TEST_FIXTURE_IDS_H