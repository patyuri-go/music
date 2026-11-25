#include "dto.h"

#include "ids.h"

#include <algorithm>

namespace jellyfin {
namespace {

std::string isoTime(const std::string &db_time) {
  if (db_time.empty()) {
    return std::string();
  }
  std::string out = db_time;
  std::replace(out.begin(), out.end(), ' ', 'T');
  return out;
}

Json::Value nameGuidPair(const std::string &name, const std::string &id) {
  Json::Value pair;
  pair["Name"] = name;
  pair["Id"] = id;
  return pair;
}

void setImageTag(Json::Value &item, const std::string &tag) {
  if (tag.empty()) {
    return;
  }
  Json::Value tags;
  tags["Primary"] = tag;
  item["ImageTags"] = std::move(tags);
}

} // namespace

Json::Value trackItem(
    const media_library::Track &t, const std::string &trackId,
    const std::string &albumId, const std::string &albumName,
    const std::string &filePath,
    const std::vector<std::pair<std::string, std::string>> &trackArtists,
    const std::vector<std::pair<std::string, std::string>> &albumArtists,
    const std::vector<std::pair<std::string, int>> &genres) {
  Json::Value r;
  const std::string trackName =
      t.title.empty() ? ("Track " + t.ref.entity_id.substr(0, 8)) : t.title;
  const std::string &trackGuid = trackId;
  const long long durationMs = t.duration_ms.value_or(0);
  const int bitrateKbps = t.audio.bitrate_kbps.value_or(0);
  const int channels = t.audio.channels.value_or(0);
  const int sampleRate = t.audio.sample_rate.value_or(0);
  const long long sizeBytes = t.audio.size_bytes.value_or(0);
  const long long bitrateBps = static_cast<long long>(bitrateKbps) * 1000LL;
  r["Name"] = trackName;
  r["ServerId"] = serverId();
  r["Id"] = trackGuid;
  r["Etag"] = trackGuid;
  r["DateCreated"] = isoTime(t.user_data.added_at);
  r["Container"] = t.audio.format;
  r["Path"] = filePath;
  r["RunTimeTicks"] = static_cast<Json::Int64>(durationMs * 10000LL);
  r["ProductionYear"] = t.year.value_or(0);
  r["IndexNumber"] = t.track_no.value_or(0);
  r["ParentIndexNumber"] = t.disc_no.value_or(0);
  r["SortName"] = t.title;
  r["Type"] = "Audio";
  r["MediaType"] = "Audio";
  r["IsFolder"] = false;
  r["CanDownload"] = true;
  r["PlayAccess"] = "Full";
  if (t.lyrics_available) {
    r["HasLyrics"] = true;
  }
  if (!albumId.empty()) {
    r["AlbumId"] = albumId;
    r["ParentId"] = albumId;
  }
  if (!albumName.empty()) {
    r["Album"] = albumName;
  }
  {
    Json::Value names(Json::arrayValue);
    Json::Value items(Json::arrayValue);
    for (const auto &p : trackArtists) {
      names.append(p.first);
      items.append(nameGuidPair(p.first, p.second));
    }
    if (!names.empty()) {
      r["Artists"] = names;
      r["ArtistItems"] = items;
    }
    const auto &albumList = albumArtists.empty() ? trackArtists : albumArtists;
    Json::Value albItems(Json::arrayValue);
    for (const auto &p : albumList) {
      albItems.append(nameGuidPair(p.first, p.second));
    }
    if (!albumList.empty()) {
      r["AlbumArtist"] = albumList.front().first;
      r["AlbumArtists"] = albItems;
    }
  }
  Json::Value genreNames(Json::arrayValue);
  Json::Value genreItems(Json::arrayValue);
  for (const auto &g : genres) {
    genreNames.append(g.first);
    genreItems.append(nameGuidPair(g.first, makeId(kKindGenre, g.second)));
  }
  r["Genres"] = genreNames;
  r["GenreItems"] = genreItems;
  {
    Json::Value stream;
    stream["Type"] = "Audio";
    stream["Codec"] = t.audio.format;
    stream["Index"] = 0;
    stream["IsDefault"] = true;
    stream["BitRate"] = static_cast<Json::Int64>(bitrateBps);
    stream["Channels"] = channels;
    stream["SampleRate"] = sampleRate;
    if (channels == 1) {
      stream["ChannelLayout"] = "mono";
    } else if (channels == 2) {
      stream["ChannelLayout"] = "stereo";
    }
    Json::Value streams(Json::arrayValue);
    streams.append(std::move(stream));

    Json::Value source;
    source["Id"] = trackGuid;
    source["Name"] = trackName;
    source["Path"] = filePath;
    source["Container"] = t.audio.format;
    source["Size"] = static_cast<Json::Int64>(sizeBytes);
    source["Bitrate"] = static_cast<Json::Int64>(bitrateBps);
    source["RunTimeTicks"] = static_cast<Json::Int64>(durationMs * 10000LL);
    source["SupportsDirectPlay"] = true;
    source["SupportsDirectStream"] = true;
    source["SupportsTranscoding"] = false;
    source["IsRemote"] = true;
    source["Type"] = "Default";
    source["MediaStreams"] = streams;
    source["DirectStreamUrl"] =
        "/Audio/" + trackGuid + "/stream?static=true&mediaSourceId=" +
        trackGuid;
    Json::Value sources(Json::arrayValue);
    sources.append(std::move(source));
    r["MediaSources"] = sources;
    r["MediaSourceCount"] = 1;
  }
  const std::string imageTag = t.artwork.has_value() ? "1" : "";
  setImageTag(r, imageTag);
  Json::Value ud;
  ud["PlaybackPositionTicks"] = 0;
  ud["PlayCount"] = t.user_data.play_count;
  ud["IsFavorite"] = false;
  ud["Played"] = t.user_data.play_count > 0;
  ud["Key"] = trackGuid;
  if (!t.user_data.last_played.empty()) {
    ud["LastPlayedDate"] = isoTime(t.user_data.last_played);
  }
  ud["Rating"] = t.user_data.rating;
  r["UserData"] = std::move(ud);
  return r;
}

Json::Value albumItem(
    const media_library::Album &a, const std::string &albumId,
    const std::vector<std::pair<std::string, std::string>> &albumArtists,
    int songCount, const std::string &imageTag, long long runtimeTicks) {
  Json::Value r;
  r["Name"] = a.title;
  r["ServerId"] = serverId();
  r["Id"] = albumId;
  r["Etag"] = albumId;
  r["Type"] = "MusicAlbum";
  r["MediaType"] = "Audio";
  r["IsFolder"] = true;
  if (!albumArtists.empty()) {
    r["AlbumArtist"] = albumArtists.front().first;
  }
  r["ProductionYear"] = a.year.value_or(0);
  r["ChildCount"] = songCount;
  r["SongCount"] = songCount;
  r["RecursiveItemCount"] = songCount;
  if (runtimeTicks > 0) {
    r["RunTimeTicks"] = static_cast<Json::Int64>(runtimeTicks);
    r["CumulativeRunTimeTicks"] = static_cast<Json::Int64>(runtimeTicks);
  }
  if (!albumArtists.empty()) {
    Json::Value names(Json::arrayValue);
    Json::Value items(Json::arrayValue);
    for (const auto &p : albumArtists) {
      names.append(p.first);
      items.append(nameGuidPair(p.first, p.second));
    }
    r["Artists"] = names;
    r["AlbumArtists"] = items;
    r["ArtistItems"] = items;
  }
  r["Genres"] = Json::Value(Json::arrayValue);
  r["GenreItems"] = Json::Value(Json::arrayValue);
  setImageTag(r, imageTag);
  Json::Value ud;
  ud["PlaybackPositionTicks"] = 0;
  ud["PlayCount"] = 0;
  ud["IsFavorite"] = false;
  ud["Played"] = false;
  r["UserData"] = std::move(ud);
  return r;
}

Json::Value artistItem(const media_library::Artist &a,
                              const std::string &artistId, int albumCount,
                              const std::string &imageTag) {
  Json::Value r;
  r["Name"] = a.name;
  r["ServerId"] = serverId();
  r["Id"] = artistId;
  r["Etag"] = artistId;
  r["Type"] = "MusicArtist";
  r["IsFolder"] = true;
  r["AlbumCount"] = albumCount;
  r["ChildCount"] = albumCount;
  r["SortName"] = a.sort_name.empty() ? a.name : a.sort_name;
  setImageTag(r, imageTag);
  return r;
}

Json::Value genreItem(int id, const std::string &name) {
  Json::Value r;
  r["Name"] = name;
  r["ServerId"] = serverId();
  r["Id"] = makeId(kKindGenre, id);
  r["Type"] = "MusicGenre";
  r["IsFolder"] = true;
  return r;
}

Json::Value userDto() {
  Json::Value r;
  r["Name"] = userName();
  r["ServerId"] = serverId();
  r["ServerName"] = serverName();
  r["Id"] = userId();
  r["HasPassword"] = false;
  r["HasConfiguredPassword"] = false;
  r["HasConfiguredEasyPassword"] = false;
  r["EnableAutoLogin"] = true;

  Json::Value config;
  config["AudioLanguagePreference"] = "";
  config["PlayDefaultAudioTrack"] = true;
  config["SubtitleLanguagePreference"] = "";
  config["DisplayMissingEpisodes"] = false;
  config["SubtitleMode"] = "Default";
  config["DisplayCollectionsView"] = false;
  config["EnableLocalPassword"] = false;
  config["HidePlayedInLatest"] = false;
  config["RememberAudioSelections"] = true;
  config["RememberSubtitleSelections"] = true;
  config["EnableNextEpisodeAutoPlay"] = false;
  r["Configuration"] = std::move(config);

  Json::Value policy;
  policy["IsAdministrator"] = true;
  policy["IsHidden"] = false;
  policy["IsDisabled"] = false;
  policy["EnableMediaPlayback"] = true;
  policy["EnableAudioPlaybackTranscoding"] = true;
  policy["EnableVideoPlaybackTranscoding"] = false;
  policy["EnablePlaybackRemuxing"] = true;
  policy["EnableContentDownloading"] = true;
  policy["EnableAllFolders"] = true;
  policy["EnableAllDevices"] = true;
  policy["EnableAllChannels"] = true;
  policy["EnableRemoteAccess"] = true;
  policy["EnableSharedDeviceControl"] = true;
  policy["EnableUserPreferenceAccess"] = true;
  policy["EnableLiveTvAccess"] = false;
  policy["EnableLiveTvManagement"] = false;
  policy["EnableContentDeletion"] = false;
  policy["EnableSyncTranscoding"] = false;
  policy["EnableMediaConversion"] = false;
  policy["InvalidLoginAttemptCount"] = 0;
  policy["LoginAttemptsBeforeLockout"] = -1;
  policy["MaxActiveSessions"] = 0;
  policy["AuthenticationProviderId"] = "Jellyfin.Server.Implementations.Users.DefaultAuthenticationProvider";
  policy["PasswordResetProviderId"] = "Jellyfin.Server.Implementations.Users.DefaultPasswordResetProvider";
  r["Policy"] = std::move(policy);
  return r;
}

Json::Value sessionInfo(const std::string &client,
                               const std::string &device) {
  Json::Value r;
  r["Id"] = "musicd-session";
  r["UserId"] = userId();
  r["UserName"] = userName();
  r["Client"] = client.empty() ? "musicd" : client;
  r["DeviceName"] = device.empty() ? "musicd" : device;
  r["ApplicationVersion"] = "1.0.0";
  r["ServerId"] = serverId();
  r["IsActive"] = true;
  r["SupportsMediaControl"] = true;
  r["SupportsRemoteControl"] = false;
  r["PlayableMediaTypes"] = Json::Value(Json::arrayValue);
  r["NowPlayingQueue"] = Json::Value(Json::arrayValue);
  r["SupportedCommands"] = Json::Value(Json::arrayValue);
  return r;
}

Json::Value playlistItem(const media_library::Playlist &p,
                                const std::string &playlistId, int childCount,
                                long long runtimeTicks) {
  Json::Value r;
  r["Name"] = p.name;
  r["ServerId"] = serverId();
  r["Id"] = playlistId;
  r["Type"] = "Playlist";
  r["MediaType"] = "Audio";
  r["IsFolder"] = true;
  r["ChildCount"] = childCount;
  if (!p.description.empty()) {
    r["Overview"] = p.description;
  }
  if (runtimeTicks > 0) {
    r["RunTimeTicks"] = static_cast<Json::Int64>(runtimeTicks);
    r["CumulativeRunTimeTicks"] = static_cast<Json::Int64>(runtimeTicks);
  }
  return r;
}

Json::Value playlistDto(const media_library::Playlist &p,
                               const std::string &playlistId) {
  Json::Value ids(Json::arrayValue);
  long long runtimeTicks = 0;
  for (const media_library::Track &track : p.tracks) {
    ids.append(itemId(track.ref));
    if (track.duration_ms.has_value()) {
      runtimeTicks += *track.duration_ms * 10000LL;
    }
  }
  Json::Value r = playlistItem(
      p, playlistId, static_cast<int>(ids.size()), runtimeTicks);
  r["ItemIds"] = std::move(ids);
  return r;
}

Json::Value queryResult(std::vector<Json::Value> items, int total,
                               int startIndex) {
  Json::Value r;
  Json::Value arr(Json::arrayValue);
  for (Json::Value &item : items) {
    arr.append(std::move(item));
  }
  r["Items"] = std::move(arr);
  r["TotalRecordCount"] = total;
  r["StartIndex"] = startIndex;
  return r;
}

} // namespace jellyfin
