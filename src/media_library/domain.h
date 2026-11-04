#ifndef MEDIA_LIBRARY_DOMAIN_H
#define MEDIA_LIBRARY_DOMAIN_H

#include "media_library/error.h"
#include "media_library/source_ref.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace media_library {

struct ArtistLink {
  ArtistRef ref;
  std::string name;
};

struct AlbumArtistLink {
  AlbumArtistRef ref;
  std::string name;
};

struct AlbumLink {
  AlbumRef ref;
  std::string title;
};

struct ArtworkRef {
  SourceRef owner;
  std::string kind{"primary"};
  std::optional<std::string> artwork_id;
  std::string cache_tag;
};

struct Restriction {
  RestrictionCode code = RestrictionCode::Unknown;
  std::string message;
};

struct UserData {
  int play_count = 0;
  std::string last_played;
  int rating = 0;
  std::string added_at;
};

struct AudioInfo {
  std::optional<int> bitrate_kbps;
  std::optional<int> sample_rate;
  std::optional<int> channels;
  std::string format;
  std::optional<std::int64_t> size_bytes;
};

struct Track {
  TrackRef ref;
  std::string title;
  std::vector<ArtistLink> artists;
  std::vector<AlbumArtistLink> album_artists;
  std::optional<AlbumLink> album;
  std::optional<std::string> date;
  std::optional<std::int64_t> duration_ms;
  std::optional<int> track_no;
  std::optional<int> disc_no;
  std::optional<int> year;
  std::vector<std::string> genres;
  std::optional<ArtworkRef> artwork;
  bool lyrics_available = false;
  bool playable = true;
  std::vector<Restriction> restrictions;
  UserData user_data;
  AudioInfo audio;
};

struct Album {
  AlbumRef ref;
  std::string title;
  std::vector<AlbumArtistLink> artists;
  std::optional<std::string> date;
  std::optional<int> year;
  std::vector<std::string> genres;
  std::optional<ArtworkRef> artwork;
};

struct Artist {
  ArtistRef ref;
  std::string name;
  std::string sort_name;
  std::optional<ArtworkRef> artwork;
};

struct Playlist {
  PlaylistRef ref;
  std::string name;
  std::string description;
  // 曲目给完整领域模型而不是 TrackRef：列表要曲目数/时长，条目端点要直接
  // 出 DTO；只留 ref 会逼上层逐条回查，等于把远端请求放大成 N 次。
  std::vector<Track> tracks;
};

enum class LyricsMode { WordSynced, LineSynced, Unsynced };

struct LyricWord {
  std::int64_t start_ms = 0;
  std::optional<std::int64_t> end_ms;
  std::string text;
};

// 辅助轨：挂在正文行上的第二语言文本。
// 平台把「译文」与「注音」分两条轨给出，二者都按正文行的时间对齐，
// 所以不是独立的时间轴，而是 line.aux 里的一条附属内容。
enum class LyricAuxKind {
  Translation,  // 译文（双语歌词的另一半）
  Romanization, // 注音（romalrc / ruby，逐字读音）
};

struct LyricAux {
  LyricAuxKind kind = LyricAuxKind::Translation;
  std::string text;
  // 逐字轨（ytlrc / 逐词注音）才有值；行级轨为空。
  std::vector<LyricWord> words;
};

struct LyricLine {
  std::optional<std::int64_t> start_ms;
  std::optional<std::int64_t> end_ms;
  std::string text;
  std::vector<LyricWord> words;
  // 同一行上可能同时有译文和注音，顺序按加入先后。
  std::vector<LyricAux> aux;
};

struct Lyrics {
  TrackRef track;
  std::optional<std::string> language;
  LyricsMode mode = LyricsMode::Unsynced;
  std::vector<LyricLine> lines;
  std::optional<std::string> plain_text;
};

enum class PlaybackKind { LocalFile, WebDav, RemoteStream, Cache };

enum class AccessMode { Proxy, Redirect };

struct PlaybackSource {
  TrackRef track;
  PlaybackKind kind = PlaybackKind::LocalFile;
  std::optional<std::string> quality;
  std::optional<int> bitrate_kbps;
  std::string mime_type;
  std::optional<std::int64_t> content_length;
  bool range_supported = true;
  std::optional<std::int64_t> expires_at;
  AccessMode access_mode = AccessMode::Proxy;
  bool cacheable = false;
  std::vector<Restriction> restrictions;
  std::string internal_locator;
};

struct PageRequest {
  std::optional<std::string> cursor;
  int limit = 50;
  // 仅聚合层使用：跳过前 offset 条再取 limit 条。与 cursor 互斥，单源库
  // 不解释该字段（聚合层会把它重放成来源的分页请求）。
  int offset = 0;
};

template <typename T> struct Page {
  std::vector<T> items;
  std::optional<std::string> next_cursor;
  bool has_more = false;
};

struct SearchQuery {
  std::string text;
  std::vector<EntityType> types;
  std::vector<std::string> artists;
  std::vector<std::string> album_artists;
  PageRequest page;
  std::optional<std::string> locale;
  bool include_unplayable = false;
  // 仅聚合层使用：限定单个库；空表示所有候选库。
  std::optional<std::string> library_id;
};

struct SearchHit {
  EntityType kind = EntityType::Track;
  SourceRef ref;
  std::string title;
  std::optional<std::string> subtitle;
  std::vector<ArtistLink> artists;
  std::vector<AlbumArtistLink> album_artists;
  // 曲目命中带上所属专辑：上层据此把列表条目连到可打开的专辑页，
  // 避免「点进去什么都没有」。专辑/艺术家命中本字段为空。
  std::optional<AlbumLink> album;
  std::optional<ArtworkRef> artwork;
  std::optional<std::int64_t> duration_ms;
  bool playable = true;
};

} // namespace media_library

#endif // MEDIA_LIBRARY_DOMAIN_H
