#ifndef MEDIA_LIBRARY_NETEASE_LIBRARY_H
#define MEDIA_LIBRARY_NETEASE_LIBRARY_H

#include "api/api-enhanced/client.h"
#include "api/api-enhanced/transport.h"
#include "media_library/domain.h"
#include "media_library/error.h"
#include "media_library/library.h"
#include "media_library/registry.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace media_library {

// 网易云媒体库对象。
//
// 与本地库的关键差异：没有 EntityStore。entity_id 就是平台 ID 的十进制
// 文本（见 api/api-enhanced/ids.h），因此 internal_id_of 语义上不成立。
//
// 阶段分工：N1 落地身份/能力/健康；N2 打开只读目录、搜索与播放直链解析
// （播放解析提前到 N2，是为了不让 stream_resolve 变成「声明了但做不到」
// 的假承诺）；N4 补歌词与封面字节；N5 补歌单。
class NeteaseLibrary : public Library,
                       public CatalogReader,
                       public Searcher,
                       public StreamResolver,
                       public LyricsProvider,
                       public ArtworkProvider,
                       public ArtworkBytes,
                       public PlaylistReader,
                       public std::enable_shared_from_this<NeteaseLibrary> {
public:
  NeteaseLibrary(std::string library_id, std::string display_name,
                 std::shared_ptr<api_enhanced::NeteaseClient> client,
                 std::string uid = {});

  // 必须在 make_shared 之后调用：内部要 shared_from_this 注册能力。
  void initialize();

  LibraryIdentity identity() const override;
  CapabilitySet capabilities() const override;
  void close() noexcept override;

  // CatalogReader
  Result<Track> get_track(const TrackRef &ref,
                          const CallContext &ctx) override;
  Result<Album> get_album(const AlbumRef &ref,
                          const CallContext &ctx) override;
  Result<Artist> get_artist(const ArtistRef &ref,
                            const CallContext &ctx) override;
  Result<Page<Track>> list_album_tracks(const AlbumRef &ref,
                                        const PageRequest &page,
                                        const CallContext &ctx) override;
  Result<Page<Album>> list_artist_albums(const ArtistRef &ref,
                                         const PageRequest &page,
                                         const CallContext &ctx) override;

  // Searcher
  Result<Page<SearchHit>> search(const SearchQuery &query,
                                 const CallContext &ctx) override;

  // StreamResolver：给出直链描述，locator 只进会话表，不进 HTTP 响应。
  Result<PlaybackSource> resolve_playback(const TrackRef &ref,
                                          const CallContext &ctx) override;

  // N4：歌词（§9）与封面（§5）。
  Result<Lyrics> get_lyrics(const TrackRef &ref,
                            const CallContext &ctx) override;
  Result<ArtworkRef> get_artwork(const SourceRef &owner,
                                 const CallContext &ctx) override;

  // N5：歌单（§4.5）。tracks 只读，不回写 playlist 表。
  Result<Page<Playlist>> list_playlists(const PageRequest &page,
                                        const CallContext &ctx) override;
  Result<Playlist> get_playlist(const PlaylistRef &ref,
                                const CallContext &ctx) override;

  // 封面字节出口：locator 是远端 http 图片地址，只进 ArtworkService。
  Result<ArtworkLocation> artwork_location(const ArtworkRef &ref,
                                           const CallContext &ctx) override;

  // 探测一次登录状态并写入内部健康快照，供 media_runtime 作为注册初始健康。
  // 不抛异常：探测失败只是把库标成 Unavailable。
  void probe_health(const CallContext &ctx);

  HealthSnapshot health() const;

private:
  // 平台响应 → 领域对象。album 参数可空：曲目自带的 al{} 已经够用。
  Track track_from_song(const api_enhanced::NeteaseSong &song,
                        const std::optional<api_enhanced::NeteaseAlbumBrief>
                            &album,
                        const CallContext &ctx) const;
  Album album_from_brief(const api_enhanced::NeteaseAlbumBrief &brief) const;
  Artist artist_from_brief(const api_enhanced::NeteaseArtist &brief) const;

  // 登录态快照：只用于把 fee==1 翻译成 playable/restriction。
  std::optional<api_enhanced::NeteaseLoginStatus> login_snapshot() const;
  bool member_active() const;

  // 按 owner 类型取封面 URL：专辑/艺术家一次请求，曲目走 song/detail。
  // 取不到返回 nullopt，由调用方翻译成 NotFound。
  std::optional<std::string> picture_of(const SourceRef &owner,
                                        const CallContext &ctx) const;

  std::string library_id_;
  std::string display_name_;
  std::shared_ptr<api_enhanced::NeteaseClient> client_;
  // 「我的歌单」需要的用户 id；留空则 list_playlists 明确报 InvalidRequest。
  std::string uid_;

  mutable std::mutex mutex_;
  HealthSnapshot health_;
  std::optional<api_enhanced::NeteaseLoginStatus> login_;
};

// 构造网易云库：建 transport + client，探测登录状态并落入初始健康。
// 只有 api_base 非法这类「配置错误」才返回 failure；api-enhanced 不可用
// 不是构造失败，而是把库标成 Unavailable 后照常注册。
Result<std::shared_ptr<NeteaseLibrary>>
make_netease_library(const std::string &library_id,
                     const std::string &display_name,
                     const api_enhanced::NeteaseTransportOptions &transport,
                     const api_enhanced::NeteaseClientOptions &client,
                     const std::string &uid = {});

} // namespace media_library

#endif // MEDIA_LIBRARY_NETEASE_LIBRARY_H
