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

crow::json::wvalue nameGuidPair(const std::string &name, const std::string &id) {
  crow::json::wvalue pair;
  pair["Name"] = name;
  pair["Id"] = id;
  return pair;
}

void setImageTag(crow::json::wvalue &item, const std::string &tag) {
  if (tag.empty()) {
    return;
  }
  crow::json::wvalue tags;
  tags["Primary"] = tag;
  item["ImageTags"] = std::move(tags);
}

} // namespace

crow::json::wvalue trackItem(
    const media_library::Track &t, const std::string &trackId,
    const std::string &albumId, const std::string &albumName,
    const std::string &filePath,
    const std::vector<std::pair<std::string, std::string>> &trackArtists,
    const std::vector<std::pair<std::string, std::string>> &albumArtists,
    const std::vector<std::pair<std::string, int>> &genres) {
  crow::json::wvalue r;
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
  r["RunTimeTicks"] = durationMs * 10000LL;
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
    crow::json::wvalue::list names;
    crow::json::wvalue::list items;
    for (const auto &p : trackArtists) {
      names.emplace_back(p.first);
      items.push_back(nameGuidPair(p.first, p.second));
    }
    if (!names.empty()) {
      r["Artists"] = crow::json::wvalue(names);
      r["ArtistItems"] = crow::json::wvalue(items);
    }
    const auto &albumList = albumArtists.empty() ? trackArtists : albumArtists;
    crow::json::wvalue::list albItems;
    for (const auto &p : albumList) {
      albItems.push_back(nameGuidPair(p.first, p.second));
    }
    if (!albumList.empty()) {
      r["AlbumArtist"] = albumList.front().first;
      r["AlbumArtists"] = crow::json::wvalue(albItems);
    }
  }
  crow::json::wvalue::list genreNames;
  crow::json::wvalue::list genreItems;
  for (const auto &g : genres) {
    genreNames.emplace_back(g.first);
    genreItems.push_back(nameGuidPair(g.first, makeId(kKindGenre, g.second)));
  }
  r["Genres"] = crow::json::wvalue(genreNames);
  r["GenreItems"] = crow::json::wvalue(genreItems);
  {
    crow::json::wvalue stream;
    stream["Type"] = "Audio";
    stream["Codec"] = t.audio.format;
    stream["Index"] = 0;
    stream["IsDefault"] = true;
    stream["BitRate"] = bitrateBps;
    stream["Channels"] = channels;
    stream["SampleRate"] = sampleRate;
    if (channels == 1) {
      stream["ChannelLayout"] = "mono";
    } else if (channels == 2) {
      stream["ChannelLayout"] = "stereo";
    }
    crow::json::wvalue::list streams;
    streams.push_back(std::move(stream));

    crow::json::wvalue source;
    source["Id"] = trackGuid;
    source["Name"] = trackName;
    source["Path"] = filePath;
    source["Container"] = t.audio.format;
    source["Size"] = sizeBytes;
    source["Bitrate"] = bitrateBps;
    source["RunTimeTicks"] = durationMs * 10000LL;
    source["SupportsDirectPlay"] = true;
    source["SupportsDirectStream"] = true;
    source["SupportsTranscoding"] = false;
    source["IsRemote"] = true;
    source["Type"] = "Default";
    source["MediaStreams"] = crow::json::wvalue(streams);
    source["DirectStreamUrl"] =
        "/Audio/" + trackGuid + "/stream?static=true&mediaSourceId=" +
        trackGuid;
    crow::json::wvalue::list sources;
    sources.push_back(std::move(source));
    r["MediaSources"] = crow::json::wvalue(sources);
    r["MediaSourceCount"] = 1;
  }
  const std::string imageTag = t.artwork.has_value() ? "1" : "";
  setImageTag(r, imageTag);
  crow::json::wvalue ud;
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

crow::json::wvalue albumItem(
    const media_library::Album &a, const std::string &albumId,
    const std::vector<std::pair<std::string, std::string>> &albumArtists,
    int songCount, const std::string &imageTag, long long runtimeTicks) {
  crow::json::wvalue r;
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
    r["RunTimeTicks"] = runtimeTicks;
    r["CumulativeRunTimeTicks"] = runtimeTicks;
  }
  if (!albumArtists.empty()) {
    crow::json::wvalue::list names;
    crow::json::wvalue::list items;
    for (const auto &p : albumArtists) {
      names.emplace_back(p.first);
      items.push_back(nameGuidPair(p.first, p.second));
    }
    r["Artists"] = crow::json::wvalue(names);
    r["AlbumArtists"] = crow::json::wvalue(items);
    r["ArtistItems"] = crow::json::wvalue(items);
  }
  r["Genres"] = crow::json::wvalue::list{};
  r["GenreItems"] = crow::json::wvalue::list{};
  setImageTag(r, imageTag);
  crow::json::wvalue ud;
  ud["PlaybackPositionTicks"] = 0;
  ud["PlayCount"] = 0;
  ud["IsFavorite"] = false;
  ud["Played"] = false;
  r["UserData"] = std::move(ud);
  return r;
}

crow::json::wvalue artistItem(const media_library::Artist &a,
                              const std::string &artistId, int albumCount,
                              const std::string &imageTag) {
  crow::json::wvalue r;
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

crow::json::wvalue genreItem(int id, const std::string &name) {
  crow::json::wvalue r;
  r["Name"] = name;
  r["ServerId"] = serverId();
  r["Id"] = makeId(kKindGenre, id);
  r["Type"] = "MusicGenre";
  r["IsFolder"] = true;
  return r;
}

crow::json::wvalue userDto() {
  crow::json::wvalue r;
  r["Name"] = userName();
  r["ServerId"] = serverId();
  r["ServerName"] = serverName();
  r["Id"] = userId();
  r["HasPassword"] = false;
  r["HasConfiguredPassword"] = false;
  r["HasConfiguredEasyPassword"] = false;
  r["EnableAutoLogin"] = true;

  crow::json::wvalue config;
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

  crow::json::wvalue policy;
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

crow::json::wvalue sessionInfo(const std::string &client,
                               const std::string &device) {
  crow::json::wvalue r;
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
  r["PlayableMediaTypes"] = crow::json::wvalue::list{};
  r["NowPlayingQueue"] = crow::json::wvalue::list{};
  r["SupportedCommands"] = crow::json::wvalue::list{};
  return r;
}

crow::json::wvalue playlistItem(const media_library::Playlist &p,
                                const std::string &playlistId, int childCount,
                                long long runtimeTicks) {
  crow::json::wvalue r;
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
    r["RunTimeTicks"] = runtimeTicks;
    r["CumulativeRunTimeTicks"] = runtimeTicks;
  }
  return r;
}

crow::json::wvalue playlistDto(const media_library::Playlist &p,
                               const std::string &playlistId) {
  crow::json::wvalue::list ids;
  ids.reserve(p.tracks.size());
  long long runtimeTicks = 0;
  for (const media_library::Track &track : p.tracks) {
    ids.emplace_back(itemId(track.ref));
    if (track.duration_ms.has_value()) {
      runtimeTicks += *track.duration_ms * 10000LL;
    }
  }
  crow::json::wvalue r = playlistItem(
      p, playlistId, static_cast<int>(ids.size()), runtimeTicks);
  r["ItemIds"] = crow::json::wvalue(std::move(ids));
  return r;
}

crow::json::wvalue queryResult(std::vector<crow::json::wvalue> items, int total,
                               int startIndex) {
  crow::json::wvalue r;
  r["Items"] = std::move(items);
  r["TotalRecordCount"] = total;
  r["StartIndex"] = startIndex;
  return r;
}

} // namespace jellyfin
