#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <mutex>
#include <string>
#include <vector>

// 播放列表：每个歌单一个 JSON 文件（<playlist_dir>/<id>.json），不入数据库。
// 每首歌保存虚拟路径（与 DB track.path 逐字节一致）、专辑、专辑艺术家。
// 刷新时用虚拟路径匹配 DB；命中则从 DB 补全标题/艺术家/时长等信息。
namespace playlist {

struct Song {
  // 写入 JSON 的字段
  std::string path;         // 虚拟路径，与 DB track.path 完全一致（含百分号编码）
  std::string album;        // 专辑名快照
  std::string album_artist; // 专辑艺术家快照（多值以 "; " 连接）
  // 以下字段由刷新/匹配从 DB 补全，不写入 JSON
  bool matched = false;
  int track_id = 0;
  std::string title;
  std::string artist;
  long long duration_ms = 0;
};

struct Playlist {
  std::string id;
  std::string name;
  std::vector<Song> songs;
};

struct Unmatched {
  std::string playlist; // 歌单名
  Song song;
};

struct RefreshResult {
  int playlists = 0;
  int songs = 0;
  int matched = 0;
  std::vector<Unmatched> unmatched;
};

// 从磁盘目录读入内存并匹配 DB。
void loadAll();
// 重新读盘 + 匹配 DB，返回统计（未匹配项一并返回）。
RefreshResult refresh();
// 内存快照（已匹配）。
std::vector<Playlist> list();
bool get(const std::string &id, Playlist &out);

// 写操作：改内存并落盘。id 为 32 位十六进制字符串。
Playlist create(const std::string &name);
bool remove(const std::string &id);
bool rename(const std::string &id, const std::string &name);
bool addPath(const std::string &id, const std::string &path, std::string *err);
bool addTrack(const std::string &id, int track_id, std::string *err);
bool removeAt(const std::string &id, int index);
// 移除歌单中所有匹配该 track_id 的曲目（不存在返回 false）。
bool removeTrack(const std::string &id, int track_id);
// 清空歌单曲目（保留歌单本身）。
bool clear(const std::string &id);

int songCount(const Playlist &p);
long long totalDurationMs(const Playlist &p);

} // namespace playlist

#endif
