#ifndef MEDIA_LIBRARY_PLAYLIST_DIRECTORY_H
#define MEDIA_LIBRARY_PLAYLIST_DIRECTORY_H

#include "media_library/domain.h"
#include "media_library/library.h"
#include "media_library/media_router.h"
#include "media_library/registry.h"

#include <memory>

namespace media_library {

// 播放列表总管。与 Router 平级，不是 Router 的成员。
//
// 职责只有两件：
//   1. list_playlists：依次向「声明了 playlist_read 的库」要一段歌单列表，
//      按库的自然顺序虚拟拼接成一页返回；
//   2. get_playlist：按 PlaylistRef 自带的 library_id 交给该库展开内容。
//
// 边界（有意不做）：
//   - 不写：没有 PlaylistWriter 能力，写操作仍不支持。
//   - 不排序：不读 library_priority，不按名字/时间重排，只按库的自然顺序。
//   - 不改引用：返回的 Playlist.ref 仍是各库自己的引用，不改写、不换源。
//   - 不合并内容：歌单本身各归各库，总管不跨库合并曲目。
//   - 不全量扫描：每库每次只要 limit 条，翻页靠游标推进。
class PlaylistDirectory {
public:
  explicit PlaylistDirectory(std::shared_ptr<LibraryRegistry> registry);

  // 跨库汇总一页。cursor 是本对象自己签发的不透明串，见 .cpp 的编码说明。
  // 所有库都失败时返回最优错误；没有库声明该能力时返回空页（不是错误）。
  Result<RoutedPage<Playlist>> list_playlists(const PageRequest &page,
                                              const CallContext &ctx);

  // 单源展开：ref 自带 library_id，不遍历、不回退、不换源。
  Result<Sourced<Playlist>> get_playlist(const PlaylistRef &ref,
                                         const CallContext &ctx);

private:
  std::shared_ptr<LibraryRegistry> registry_;
};

std::shared_ptr<PlaylistDirectory>
make_playlist_directory(std::shared_ptr<LibraryRegistry> registry);

} // namespace media_library

#endif // MEDIA_LIBRARY_PLAYLIST_DIRECTORY_H
