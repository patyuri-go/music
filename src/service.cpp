#include "service.h"

#include "config.h"
#include "media_library/domain.h"
#include "media_library/library_runtime.h"
#include "media_library/snapshot.h"

#include "crow/json.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

crow::json::wvalue intList(const std::vector<int> &ids) {
  crow::json::wvalue::list list;
  for (const int id : ids) {
    list.emplace_back(id);
  }
  return crow::json::wvalue(list);
}

// 旧版 /api 响应需要路径、mtime、内部 ID 等传输细节；
// 领域模型只提供语义字段，这里在兼容边界把它们拼回去。
crow::json::wvalue trackToJson(int internal_id, const media_library::Track &t,
                               const std::optional<media_library::TrackTransport> &transport) {
  crow::json::wvalue out;
  out["id"] = internal_id;
  out["title"] = t.title;
  out["path"] = transport ? transport->path : std::string();
  out["artist_ids"] = intList(transport ? transport->artist_internal_ids
                                        : std::vector<int>{});
  out["album_id"] = transport ? transport->album_internal_id : 0;
  out["disc_no"] = t.disc_no.value_or(1);
  out["track_no"] = t.track_no.value_or(0);
  out["duration_ms"] = t.duration_ms.value_or(0);
  out["bitrate"] = t.audio.bitrate_kbps.value_or(0);
  out["sample_rate"] = t.audio.sample_rate.value_or(0);
  out["channels"] = t.audio.channels.value_or(0);
  out["format"] = t.audio.format;
  out["size_bytes"] = t.audio.size_bytes.value_or(0);
  out["mtime"] = transport ? transport->mtime : 0;
  out["added_at"] = t.user_data.added_at;
  out["play_count"] = t.user_data.play_count;
  out["last_played"] = t.user_data.last_played;
  out["rating"] = t.user_data.rating;
  return out;
}

// 按内部 ID 取单个曲目并转成旧版 JSON；不存在时返回空。
std::optional<crow::json::wvalue> trackJson(int internal_id) {
  auto &library = media_library::library();
  const auto ref = library.ref_for_compat(media_library::EntityType::Track, internal_id);
  if (!ref) {
    return std::nullopt;
  }
  const auto result = library.get_track(*ref, {});
  if (!result.ok()) {
    return std::nullopt;
  }
  return trackToJson(internal_id, result.value(),
                     library.track_transport(internal_id));
}

crow::json::wvalue tracksToList(const std::vector<int> &internal_ids) {
  crow::json::wvalue out(crow::json::type::List);
  int index = 0;
  for (const int internal_id : internal_ids) {
    auto item = trackJson(internal_id);
    if (!item) {
      continue;
    }
    out[index] = std::move(*item);
    ++index;
  }
  return out;
}

std::vector<int> sortRecent(const media_library::CatalogSnapshot &snap) {
  std::vector<std::size_t> order(snap.track_internal_ids.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    const auto &left = snap.tracks[a];
    const auto &right = snap.tracks[b];
    if (left.user_data.added_at != right.user_data.added_at) {
      return left.user_data.added_at > right.user_data.added_at;
    }
    return snap.track_internal_ids[a] > snap.track_internal_ids[b];
  });
  std::vector<int> ids;
  ids.reserve(order.size());
  for (const std::size_t i : order) {
    ids.push_back(snap.track_internal_ids[i]);
  }
  return ids;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

} // namespace

Service::Service() = default;

std::optional<crow::json::wvalue> Service::query(const crow::json::rvalue &json) {
  if (!json || !json.has("type")) {
    return std::nullopt;
  }
  const std::string &type = json["type"].s();
  if (type == "home") {
    return home(json);
  }
  if (type == "tag") {
    return tag(json);
  }
  if (type == "album") {
    return album(json);
  }
  if (type == "artist") {
    return artist(json);
  }
  if (type == "search") {
    return search(json);
  }
  return std::nullopt;
}

std::optional<crow::json::wvalue> Service::trackById(int id) {
  return trackJson(id);
}

std::optional<crow::json::wvalue> Service::albumById(int id) {
  auto &library = media_library::library();
  const auto ref = library.ref_for_compat(media_library::EntityType::Album, id);
  if (!ref) {
    return std::nullopt;
  }
  const auto result = library.get_album(*ref, {});
  if (!result.ok()) {
    return std::nullopt;
  }
  const auto &album = result.value();

  crow::json::wvalue out;
  out["id"] = id;
  out["name"] = album.title;
  std::vector<int> album_artist_ids;
  for (const auto &link : album.artists) {
    if (const auto internal = library.internal_id_of(link.ref)) {
      album_artist_ids.push_back(*internal);
    }
  }
  out["album_artists"] = intList(album_artist_ids);
  out["year"] = album.year.value_or(0);
  out["cover_path"] = library.artwork_path(album.ref).value_or(std::string());

  const auto tracks = library.list_album_tracks(
      *ref, media_library::PageRequest{std::nullopt, 1000000}, {});
  int index = 0;
  if (tracks.ok()) {
    for (const auto &track : tracks.value().items) {
      const auto internal = library.internal_id_of(track.ref);
      if (!internal) {
        continue;
      }
      out["tracks"][index] = trackToJson(*internal, track,
                                         library.track_transport(*internal));
      ++index;
    }
  }
  return out;
}

std::optional<crow::json::wvalue> Service::artistById(int id) {
  auto &library = media_library::library();
  const auto ref = library.ref_for_compat(media_library::EntityType::Artist, id);
  if (!ref) {
    return std::nullopt;
  }
  const auto result = library.get_artist(*ref, {});
  if (!result.ok()) {
    return std::nullopt;
  }
  const auto &artist = result.value();

  crow::json::wvalue out;
  out["id"] = id;
  out["name"] = artist.name;
  out["sort_name"] = artist.sort_name;

  const auto albums = library.list_artist_albums(
      *ref, media_library::PageRequest{std::nullopt, 1000000}, {});
  int index = 0;
  if (albums.ok()) {
    for (const auto &album : albums.value().items) {
      const auto internal = library.internal_id_of(album.ref);
      if (!internal) {
        continue;
      }
      std::vector<int> album_artist_ids;
      for (const auto &link : album.artists) {
        if (const auto artist_id = library.internal_id_of(link.ref)) {
          album_artist_ids.push_back(*artist_id);
        }
      }
      out["albums"][index]["id"] = *internal;
      out["albums"][index]["name"] = album.title;
      out["albums"][index]["album_artists"] = intList(album_artist_ids);
      out["albums"][index]["year"] = album.year.value_or(0);
      out["albums"][index]["cover_path"] =
          library.artwork_path(album.ref).value_or(std::string());
      ++index;
    }
  }
  return out;
}

std::optional<crow::json::wvalue> Service::home(const crow::json::rvalue &json) {
  int after_id = 0;
  int pegcount = config::get().page_size;

  if (json.has("afterid")) {
    after_id = static_cast<int>(json["afterid"].i());
  }
  if (json.has("pegcount")) {
    pegcount = static_cast<int>(json["pegcount"].i());
  }

  const auto snap = media_library::library().snapshot();
  std::vector<int> ids;
  for (const int internal_id : sortRecent(snap)) {
    if (internal_id <= after_id) {
      continue;
    }
    ids.push_back(internal_id);
    if (pegcount > 0 && static_cast<int>(ids.size()) >= pegcount) {
      break;
    }
  }
  return tracksToList(ids);
}

std::optional<crow::json::wvalue> Service::tag(const crow::json::rvalue &json) {
  if (!json.has("tag")) {
    return crow::json::wvalue(crow::json::type::List);
  }
  const std::string tag_name = json["tag"].s();
  const int page_size = config::get().page_size;

  const auto snap = media_library::library().snapshot();
  std::vector<int> ids;
  for (const int internal_id : sortRecent(snap)) {
    const auto genres = snap.track_genres.find(internal_id);
    if (genres == snap.track_genres.end()) {
      continue;
    }
    bool matched = false;
    for (const auto &genre : genres->second) {
      if (genre.name == tag_name) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      continue;
    }
    ids.push_back(internal_id);
    if (page_size > 0 && static_cast<int>(ids.size()) >= page_size) {
      break;
    }
  }
  return tracksToList(ids);
}

std::optional<crow::json::wvalue> Service::album(const crow::json::rvalue &) {
  return std::nullopt;
}

std::optional<crow::json::wvalue> Service::artist(const crow::json::rvalue &) {
  return std::nullopt;
}

std::optional<crow::json::wvalue> Service::search(const crow::json::rvalue &json) {
  if (!json.has("q")) {
    return crow::json::wvalue(crow::json::type::List);
  }
  const std::string query = lower(json["q"].s());
  const int page_size = config::get().page_size;

  const auto snap = media_library::library().snapshot();
  std::unordered_map<int, std::size_t> index;
  for (std::size_t i = 0; i < snap.track_internal_ids.size(); ++i) {
    index.emplace(snap.track_internal_ids[i], i);
  }

  std::vector<int> ids;
  for (const int internal_id : sortRecent(snap)) {
    const auto it = index.find(internal_id);
    if (it == index.end()) {
      continue;
    }
    if (lower(snap.tracks[it->second].title).find(query) == std::string::npos) {
      continue;
    }
    ids.push_back(internal_id);
    if (page_size > 0 && static_cast<int>(ids.size()) >= page_size) {
      break;
    }
  }
  return tracksToList(ids);
}
