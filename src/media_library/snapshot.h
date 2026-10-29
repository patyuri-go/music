#ifndef MEDIA_LIBRARY_SNAPSHOT_H
#define MEDIA_LIBRARY_SNAPSHOT_H

#include "media_library/domain.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace media_library {

// 流派不是顶层实体，但浏览/筛选需要它的库内部 ID。
struct GenreEntry {
  int internal_id = 0;
  std::string name;
};

// 一次性读出的目录快照：全部是统一领域模型，不含平台或数据库行结构。
// 供浏览列表、筛选选项等聚合场景使用，避免逐条查询。
//
// internal_ids 与同名实体数组一一对应：领域模型本身不携带库内部 ID，
// 兼容层需要它把 SourceRef 映射成对外 ID 时才用这张平行表。
struct CatalogSnapshot {
  std::vector<Track> tracks;
  std::vector<Album> albums;
  std::vector<Artist> artists;
  std::vector<GenreEntry> genres;

  std::vector<int> track_internal_ids;
  std::vector<int> album_internal_ids;
  std::vector<int> artist_internal_ids;

  std::unordered_map<int, int> album_track_count;
  std::unordered_map<int, long long> album_duration_ms;
  std::unordered_map<int, int> artist_album_count;
  std::unordered_map<int, std::vector<GenreEntry>> track_genres;

  // 兼容层构造 Jellyfin MediaSource.Path 时使用；属于传输细节，不进入领域模型。
  std::unordered_map<int, std::string> track_paths;
  // 最近播放顺序（play_event.played_at DESC），元素为库内部 track ID。
  std::vector<int> recent_play_track_ids;
};

// 旧版 /api 响应需要的传输细节：路径、mtime、库内部 ID。
// 不属于统一领域模型，只供兼容门面拼装 JSON。
struct TrackTransport {
  std::string path;
  long long mtime = 0;
  std::vector<int> artist_internal_ids;
  int album_internal_id = 0;
};
} // namespace media_library

#endif // MEDIA_LIBRARY_SNAPSHOT_H
