#ifndef METADATA_H
#define METADATA_H

#include <optional>
#include <string>
#include <vector>

namespace metadata {

/// 按常见分隔符（; 和 /）拆分多值标签，去除空白并去重（保持顺序）。
std::vector<std::string> splitArtists(const std::string &value);

struct TrackMeta {
  std::string title;
  std::string artist;
  std::vector<std::string> artists;
  std::string album;
  std::vector<std::string> album_artists;
  int year = 0;
  std::string genre;
  int track_no = 0;
  int disc_no = 1;
  int duration_ms = 0;
  int bitrate = 0;
  int sample_rate = 0;
  int channels = 0;
  std::string format;
  long long size_bytes = 0;
  long long mtime = 0;
  std::string lyrics; ///< 内嵌歌词（若有）。
};

std::optional<TrackMeta> read(const std::string &path);

/// 读取与给定音频路径同名（同目录、同 stem）的 .lrc 歌词文件；
/// 本地路径读文件，WebDAV URL 走远端 GET。找不到或失败返回空串。
std::string readSiblingLyrics(const std::string &path);

/// 判断歌词文本是否为带时间轴的 LRC（含 [mm:ss] 形式时间标签）。
bool isSyncedLyrics(const std::string &text);

struct FileMeta {
  std::string title;
  std::string artist;
  std::vector<std::string> artists;
  std::string album;
  std::vector<std::string> album_artists;
  std::string genre;
  int year = 0;
  std::string comment;
  int track_no = 0;
  int disc_no = 1;
  long long size_bytes = 0;
  int bitrate = 0;
  int sample_rate = 0;
  int channels = 0;
  std::string format;
  std::string lyrics; ///< 内嵌歌词（若有）。
};

std::optional<FileMeta> readFileMeta(const std::string &path);
bool writeFileMeta(const std::string &path, const FileMeta &meta);

} // namespace metadata

#endif
