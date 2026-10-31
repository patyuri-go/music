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
  std::vector<TrackRef> tracks;
};

enum class LyricsMode { WordSynced, LineSynced, Unsynced };

struct LyricWord {
  std::int64_t start_ms = 0;
  std::optional<std::int64_t> end_ms;
  std::string text;
};

struct LyricLine {
  std::optional<std::int64_t> start_ms;
  std::optional<std::int64_t> end_ms;
  std::string text;
  std::vector<LyricWord> words;
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
