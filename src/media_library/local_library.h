#ifndef MEDIA_LIBRARY_LOCAL_LIBRARY_H
#define MEDIA_LIBRARY_LOCAL_LIBRARY_H

#include "media_library/domain.h"
#include "media_library/entity_store.h"
#include "media_library/library.h"
#include "media_library/snapshot.h"

#include "database.h"

#include <optional>
#include <string>
#include <vector>

namespace media_library {

// 把现有本地数据库、文件存储和歌词/封面读取封装成一个 Library 对象。
// 只实现只读能力；不写 HTTP，不返回平台 DTO，不传音频字节。
class LocalLibrary : public Library,
                     public CatalogReader,
                     public Searcher,
                     public StreamResolver,
                     public LyricsProvider,
                     public ArtworkProvider,
                     public ArtworkBytes,
                     public std::enable_shared_from_this<LocalLibrary> {
public:
  LocalLibrary(std::string library_id, std::string display_name,
               std::string db_path);

  // 必须在 make_shared 之后调用：注册能力接口并补齐实体映射。
  void initialize();

  LibraryIdentity identity() const override;
  CapabilitySet capabilities() const override;
  void close() noexcept override;

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

  Result<Page<SearchHit>> search(const SearchQuery &query,
                                 const CallContext &ctx) override;

  Result<PlaybackSource> resolve_playback(const TrackRef &ref,
                                          const CallContext &ctx) override;

  Result<Lyrics> get_lyrics(const TrackRef &ref,
                            const CallContext &ctx) override;

  Result<ArtworkRef> get_artwork(const SourceRef &owner,
                                 const CallContext &ctx) override;

  // 把封面描述变成可打开的字节位置（ArtworkService 消费）。
  // `locator` 与 cache_virtual_path 都是内部定位信息，不出进程。
  Result<ArtworkLocation> artwork_location(const ArtworkRef &ref,
                                           const CallContext &ctx) override;

  // 封面文件路径；仅传输层使用。
  std::optional<std::string> artwork_path(const SourceRef &owner) const;

  // 当前 Jellyfin 兼容 id 直接编码库内部 id；过渡期用它换成 SourceRef。
  std::optional<SourceRef> ref_for_compat(EntityType type, int internal_id);

  SourceRef ref_for(EntityType type, int internal_id);

  // 兼容层把 SourceRef 映射成对外 ID 时需要反查库内部 ID。
  std::optional<int> internal_id_of(const SourceRef &ref) const;

  // 一次性读出全库目录快照，供浏览列表和筛选选项使用。
  // 返回领域模型，不暴露数据库行结构。
  CatalogSnapshot snapshot();

  // 旧版 /api 响应需要的传输细节（路径、mtime、内部 ID）。
  // 只供兼容门面拼装 JSON；领域模型不携带这些字段。
  std::optional<TrackTransport> track_transport(int internal_id) const;

private:
  Track build_track(const repo::Track &row);
  Album build_album(const repo::Album &row);
  Artist build_artist(const repo::Artist &row);
  std::vector<std::string> genres_of(int track_id) const;
  std::optional<repo::Album> album_of(const repo::Track &row) const;
  std::string cover_path_of(const SourceRef &owner) const;

  std::string library_id_;
  std::string display_name_;
  std::string db_path_;
  EntityStore entities_;
};

} // namespace media_library

#endif // MEDIA_LIBRARY_LOCAL_LIBRARY_H
