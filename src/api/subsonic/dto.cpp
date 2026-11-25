#include "api/subsonic/dto.h"

#include "media_library/source_ref.h"

#include <algorithm>

namespace subsonic {
namespace {

std::string id_of(const media_library::SourceRef &ref) {
  return media_library::to_string(ref);
}

// Subsonic 的时间格式：ISO 8601。领域模型只给了字符串（本地是扫描时间，
// 远端是平台原样字符串），能原样用就原样用。
std::string iso_time(const std::string &value) { return value; }

// 封面 id：Subsonic 用 `getCoverArt?id=` 取图，这里给实体自身 ref。
// 没有封面时不写 coverArt 字段（客户端会画占位图）。
void attach_cover(Json::Value &node,
                  const std::optional<media_library::ArtworkRef> &artwork) {
  if (!artwork.has_value()) {
    return;
  }
  node["coverArt"] = id_of(artwork->owner);
}

std::string join_names(const std::vector<media_library::ArtistLink> &artists) {
  std::string out;
  for (const auto &artist : artists) {
    if (!out.empty()) {
      out += ", ";
    }
    out += artist.name;
  }
  return out;
}

std::string join_album_artist_names(
    const std::vector<media_library::AlbumArtistLink> &artists) {
  std::string out;
  for (const auto &artist : artists) {
    if (!out.empty()) {
      out += ", ";
    }
    out += artist.name;
  }
  return out;
}

// OpenSubsonic 的 ArtistID3 最小形态：id + name。
// 多位艺术家必须是数组里的多个对象，不能塞进一个 id/name。
template <typename Link>
Json::Value artist_link_dto(const Link &link) {
  Json::Value node;
  node["id"] = id_of(link.ref);
  node["name"] = link.name;
  return node;
}

} // namespace

int duration_seconds(const media_library::Track &track) {
  if (!track.duration_ms.has_value()) {
    return 0;
  }
  return static_cast<int>(*track.duration_ms / 1000);
}

Json::Value song_dto(const media_library::Track &track) {
  Json::Value node;
  node["id"] = id_of(track.ref);
  node["parent"] = track.album.has_value() ? id_of(track.album->ref)
                                           : id_of(track.ref);
  node["isDir"] = false;
  node["title"] = track.title;
  node["album"] = track.album.has_value() ? track.album->title : std::string();

  const std::string artist = join_names(track.artists);
  node["artist"] = artist;
  // OpenSubsonic：artists 是数组，客户端据此把每位艺术家渲染成独立链接。
  // 只给 artist 字符串时，多人合作会被当成一个合并后的艺术家。
  if (!track.artists.empty()) {
    Json::Value artists(Json::arrayValue);
    for (const auto &link : track.artists) {
      artists.append(artist_link_dto(link));
    }
    node["artists"] = std::move(artists);
    node["displayArtist"] = artist;
  }
  // 没有专辑艺术家就整个字段不输出，不要拿艺术家顶替。
  const std::string album_artist = join_album_artist_names(track.album_artists);
  if (!album_artist.empty()) {
    node["albumArtist"] = album_artist;
    Json::Value album_artists(Json::arrayValue);
    for (const auto &link : track.album_artists) {
      album_artists.append(artist_link_dto(link));
    }
    node["albumArtists"] = std::move(album_artists);
    node["displayAlbumArtist"] = album_artist;
  }

  if (track.track_no.has_value()) {
    node["track"] = *track.track_no;
  }
  if (track.disc_no.has_value()) {
    node["discNumber"] = *track.disc_no;
  }
  if (track.year.has_value()) {
    node["year"] = *track.year;
  }
  if (track.date.has_value()) {
    node["created"] = iso_time(*track.date);
  }
  node["duration"] = duration_seconds(track);
  // Subsonic 的 Child 用 albumId / artistId 做「跳到专辑」「跳到艺术家」目标，
  // 只给 parent 不够：多数客户端点歌手按钮读的是 artistId。
  if (track.album.has_value()) {
    node["albumId"] = id_of(track.album->ref);
  }
  if (!track.album_artists.empty()) {
    node["artistId"] = id_of(track.album_artists.front().ref);
  } else if (!track.artists.empty()) {
    node["artistId"] = id_of(track.artists.front().ref);
  }
  if (track.audio.bitrate_kbps.has_value()) {
    node["bitRate"] = *track.audio.bitrate_kbps;
  }
  if (track.audio.size_bytes.has_value()) {
    node["size"] = *track.audio.size_bytes;
  }
  if (!track.audio.format.empty()) {
    node["suffix"] = track.audio.format;
    node["contentType"] = "audio/" + track.audio.format;
  }
  if (track.audio.channels.has_value()) {
    node["channelCount"] = *track.audio.channels;
  }
  node["isVideo"] = false;
  node["type"] = "music";
  node["playCount"] = track.user_data.play_count;

  // 不可播放（VIP / 版权 / 地区）要如实标出来，客户端才不会反复点。
  node["isPlayable"] = track.playable;
  if (!track.playable) {
    node["playableMessage"] = track.restrictions.empty()
                                  ? std::string("not playable")
                                  : track.restrictions.front().message;
  }

  attach_cover(node, track.artwork);
  return node;
}

Json::Value album_dto(const media_library::Album &album,
                             bool include_songs) {
  Json::Value node;
  node["id"] = id_of(album.ref);
  node["name"] = album.title;
  // 专辑对象在 Subsonic 里叫 album，但 OpenSubsonic 示例里同时给 title；
  // 部分客户端只读 title。
  node["album"] = album.title;
  node["title"] = album.title;
  const std::string artist = join_album_artist_names(album.artists);
  node["artist"] = artist;
  node["artistId"] = album.artists.empty()
                         ? std::string()
                         : id_of(album.artists.front().ref);
  // 专辑同样给 OpenSubsonic artists 数组，避免多人专辑只暴露合并后的字符串。
  if (!album.artists.empty()) {
    Json::Value artists(Json::arrayValue);
    for (const auto &link : album.artists) {
      artists.append(artist_link_dto(link));
    }
    node["artists"] = std::move(artists);
    node["displayArtist"] = artist;
  }
  if (album.year.has_value()) {
    node["year"] = *album.year;
  }
  if (album.date.has_value()) {
    node["created"] = iso_time(*album.date);
  }
  node["songCount"] = 0;
  node["duration"] = 0;
  node["isDir"] = true;
  attach_cover(node, album.artwork);
  if (!include_songs) {
    return node;
  }
  // getAlbum 的 song[] 由调用方经 list_album_tracks 取到后再填，
  // 这里只建空数组占位，避免下游 has() 判断分叉。
  node["song"] = Json::Value(Json::arrayValue);
  return node;
}

Json::Value artist_dto(const media_library::Artist &artist,
                              int album_count) {
  Json::Value node;
  node["id"] = id_of(artist.ref);
  node["name"] = artist.name;
  node["albumCount"] = album_count;
  if (!artist.sort_name.empty()) {
    node["sortName"] = artist.sort_name;
  }
  attach_cover(node, artist.artwork);
  return node;
}

Json::Value artist_info_dto(const media_library::ArtistInfo &info) {
  Json::Value node;
  if (!info.description.empty()) {
    node["biography"] = info.description;
  }
  node["similarArtist"] = Json::Value(Json::arrayValue);
  return node;
}

Json::Value album_info_dto(const media_library::AlbumInfo &info) {
  Json::Value node;
  if (!info.description.empty()) {
    node["notes"] = info.description;
  }
  return node;
}

Json::Value playlist_dto(const media_library::Playlist &playlist,
                                bool include_entries) {
  Json::Value node;
  node["id"] = id_of(playlist.ref);
  node["name"] = playlist.name;
  if (!playlist.description.empty()) {
    node["comment"] = playlist.description;
  }
  node["owner"] = "anonymous";
  node["public"] = true;
  node["songCount"] = static_cast<int>(playlist.tracks.size());

  long long total_ms = 0;
  for (const auto &track : playlist.tracks) {
    total_ms += track.duration_ms.value_or(0);
  }
  node["duration"] = static_cast<int>(total_ms / 1000);

  if (!include_entries) {
    return node;
  }
  Json::Value entries(Json::arrayValue);
    for (const auto &track : playlist.tracks) {
    entries.append(song_dto(track));
  }
  node["entry"] = std::move(entries);
  return node;
}

Json::Value music_folder_dto(const std::string &library_id,
                                    const std::string &name) {
  Json::Value node;
  node["id"] = library_id;
  node["name"] = name.empty() ? library_id : name;
  return node;
}

Json::Value user_dto(const std::string &username) {
  Json::Value node;
  node["username"] = username.empty() ? "admin" : username;
  node["email"] = "";
  node["scrobblingEnabled"] = false;
  // 只打开门面确实提供的能力：下载、歌单、封面。
  node["adminRole"] = false;
  node["settingsRole"] = false;
  node["downloadRole"] = true;
  node["uploadRole"] = false;
  node["playlistRole"] = true;
  node["coverArtRole"] = true;
  node["commentRole"] = false;
  node["podcastRole"] = false;
  node["shareRole"] = false;
  node["videoConversionRole"] = false;
  return node;
}

Json::Value search_hit_dto(const media_library::SearchHit &hit) {
  using media_library::EntityType;
  switch (hit.kind) {
  case EntityType::Track: {
    media_library::Track track;
    track.ref = hit.ref;
    track.title = hit.title;
    track.artists = hit.artists;
    track.album_artists = hit.album_artists;
    track.album = hit.album;
    track.artwork = hit.artwork;
    track.duration_ms = hit.duration_ms;
    track.playable = hit.playable;
    return song_dto(track);
  }
  case EntityType::Album: {
    media_library::Album album;
    album.ref = hit.ref;
    album.title = hit.title;
    album.artists = hit.album_artists;
    album.artwork = hit.artwork;
    return album_dto(album, false);
  }
  case EntityType::Artist:
  case EntityType::AlbumArtist: {
    media_library::Artist artist;
    artist.ref = hit.ref;
    artist.name = hit.title;
    artist.artwork = hit.artwork;
    return artist_dto(artist, 0);
  }
  default:
    return Json::Value(Json::nullValue);
  }
}

} // namespace subsonic
