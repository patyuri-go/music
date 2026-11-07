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

// 专辑的补充信息。与 Album 分开查询：列表页不需要简介，详情页才取。
struct AlbumInfo {
  AlbumRef ref;
  std::string description;
};

struct Artist {
  ArtistRef ref;
  std::string name;
  std::string sort_name;
  std::optional<ArtworkRef> artwork;
};

// 艺术家的补充信息。ref 用 SourceRef 保留原始类型：
// 同一个平台 ID 既可能是 Artist，也可能是 AlbumArtist。
struct ArtistInfo {
  SourceRef ref;
  std::string description;
};

struct Playlist {
  PlaylistRef ref;
  std::string name;
  std::string description;
  // 曲目给完整领域模型而不是 TrackRef：列表要曲目数/时长，条目端点要直接
  // 出 DTO；只留 ref 会逼上层逐条回查，等于把远端请求放大成 N 次。
  std::vector<Track> tracks;
};

// 歌词的时间模式与内容特征。
//
// `Lyrics::modes` 固定包含且只包含一个时间模式（WordSynced / LineSynced /
// Unsynced），其余标志按内容追加。不要假设顺序，统一用 has_mode() 查询。
enum class LyricsMode {
  WordSynced,
  LineSynced,
  Unsynced,
  HasTranslation,  // 有译文轨
  HasRomanization, // 有标音轨（罗马音 / Ruby 注音）
  HasBackground,   // 有背景人声
};

struct LyricWord {
  std::int64_t start_ms = 0;
  std::optional<std::int64_t> end_ms;
  std::string text;
};

// Ruby（注音）：base 是原文，texts 是逐段带时间的读音。
struct LyricRuby {
  std::string base;
  std::vector<LyricWord> texts;
};

// 一行歌词里的有序内容单元，对应 AMLL TTML 的不同 span。
// 顺序本身是语义（主唱、背景人声、译文可以交错），不要重排。
enum class LyricItemKind {
  Text,         // 行级纯文本
  Word,         // 逐字音节（start_ms 必填）
  Ruby,         // 注音
  Background,   // 背景人声，children 里可再带时间轴
  Translation,  // 译文
  Romanization, // 标音（罗马音）
};

struct LyricItem {
  LyricItemKind kind = LyricItemKind::Text;
  std::string text;                     // Text / Translation / Romanization
  std::optional<std::int64_t> start_ms; // Word / Background
  std::optional<std::int64_t> end_ms;
  std::optional<std::string> lang;      // Translation / Romanization
  LyricRuby ruby;                       // Ruby
  // Background 的子内容；Translation / Romanization 逐字时也放在这里。
  std::vector<LyricItem> children;
};

struct LyricLine {
  std::optional<std::int64_t> start_ms;
  std::optional<std::int64_t> end_ms;
  std::vector<LyricItem> items;
};

struct Lyrics {
  TrackRef track;
  std::optional<std::string> language;
  // 固定一个时间模式，按需追加 Has* 内容标志。
  std::vector<LyricsMode> modes{LyricsMode::Unsynced};
  std::vector<LyricLine> lines;
  std::optional<std::string> plain_text;
};

inline bool has_mode(const Lyrics &lyrics, LyricsMode mode) {
  for (const LyricsMode entry : lyrics.modes) {
    if (entry == mode) {
      return true;
    }
  }
  return false;
}

inline bool is_synced(const Lyrics &lyrics) {
  return has_mode(lyrics, LyricsMode::WordSynced) ||
         has_mode(lyrics, LyricsMode::LineSynced);
}

// 正文纯文本：拼顶层 Text / Word 与 Ruby 的 base。
// 背景人声、译文、标音是附属内容，不计入正文。
inline std::string line_text(const LyricLine &line) {
  std::string out;
  for (const LyricItem &item : line.items) {
    switch (item.kind) {
    case LyricItemKind::Text:
    case LyricItemKind::Word:
      out += item.text;
      break;
    case LyricItemKind::Ruby:
      out += item.ruby.base;
      break;
    default:
      break;
    }
  }
  return out;
}

// 顶层逐字轨。Ruby 读音不是正文音节，不混进来。
inline std::vector<LyricWord> line_words(const LyricLine &line) {
  std::vector<LyricWord> out;
  for (const LyricItem &item : line.items) {
    if (item.kind != LyricItemKind::Word) {
      continue;
    }
    LyricWord word;
    word.start_ms = item.start_ms.value_or(0);
    word.end_ms = item.end_ms;
    word.text = item.text;
    out.push_back(std::move(word));
  }
  return out;
}

// 一个 item 下的逐字子项（Translation/Romanization 的逐字轨，或
// Background 的子项）。
inline std::vector<LyricWord> item_words(const LyricItem &item) {
  std::vector<LyricWord> out;
  for (const LyricItem &child : item.children) {
    if (child.kind != LyricItemKind::Word) {
      continue;
    }
    LyricWord word;
    word.start_ms = child.start_ms.value_or(0);
    word.end_ms = child.end_ms;
    word.text = child.text;
    out.push_back(std::move(word));
  }
  return out;
}

inline const LyricItem *find_item(const LyricLine &line, LyricItemKind kind) {
  for (const LyricItem &item : line.items) {
    if (item.kind == kind) {
      return &item;
    }
  }
  return nullptr;
}

inline bool line_has_item(const LyricLine &line, LyricItemKind kind) {
  return find_item(line, kind) != nullptr;
}

// 行内第一个非空文本项（译文/标音占位可能为空）。
inline const LyricItem *find_text_item(const LyricLine &line,
                                       LyricItemKind kind) {
  for (const LyricItem &item : line.items) {
    if (item.kind == kind && !item.text.empty()) {
      return &item;
    }
  }
  return nullptr;
}

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
