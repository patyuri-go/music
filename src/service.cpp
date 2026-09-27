#include "service.h"

#include "config.h"
#include "database.h"
#include "crow/json.h"
#include "db.h"
#include "schema.h"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

crow::json::wvalue intList(const std::vector<int> &ids) {
  crow::json::wvalue::list list;
  for (const int id : ids) {
    list.emplace_back(id);
  }
  return crow::json::wvalue(list);
}

std::string databasePath() { return config::get().db_path.string(); }

std::string nowTimestamp() {
  auto now = std::chrono::system_clock::now();
  std::time_t now_c = std::chrono::system_clock::to_time_t(now);
  std::tm bt = *std::localtime(&now_c);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &bt);
  return buf;
}

crow::json::wvalue trackToJson(const repo::Track &t) {
  crow::json::wvalue out;
  out["id"] = t.id;
  out["title"] = t.title;
  out["path"] = t.path;
  out["artist_ids"] = intList(t.artist_ids);
  out["album_id"] = t.album_id;
  out["disc_no"] = t.disc_no;
  out["track_no"] = t.track_no;
  out["duration_ms"] = t.duration_ms;
  out["bitrate"] = t.bitrate;
  out["sample_rate"] = t.sample_rate;
  out["channels"] = t.channels;
  out["format"] = t.format;
  out["size_bytes"] = t.size_bytes;
  out["mtime"] = t.mtime;
  out["added_at"] = t.added_at;
  out["play_count"] = t.play_count;
  out["last_played"] = t.last_played;
  out["rating"] = t.rating;
  return out;
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
  repo::TrackRepo repo(databasePath());
  auto track = repo.byId(id);
  if (!track) {
    return std::nullopt;
  }
  return trackToJson(*track);
}

std::optional<crow::json::wvalue> Service::albumById(int id) {
  repo::AlbumRepo repo(databasePath());
  auto album = repo.byId(id);
  if (!album) {
    return std::nullopt;
  }
  crow::json::wvalue out;
  out["id"] = album->id;
  out["name"] = album->name;
  out["album_artists"] = intList(album->artist_ids);
  out["year"] = album->year;
  out["cover_path"] = album->cover_path;

  repo::TrackRepo trackRepo(databasePath());
  auto tracks = trackRepo.byAlbum(id);
  int i = 0;
  for (const auto &t : tracks) {
    out["tracks"][i] = trackToJson(t);
    ++i;
  }
  return out;
}

std::optional<crow::json::wvalue> Service::artistById(int id) {
  repo::ArtistRepo repo(databasePath());
  auto artist = repo.byId(id);
  if (!artist) {
    return std::nullopt;
  }
  crow::json::wvalue out;
  out["id"] = artist->id;
  out["name"] = artist->name;
  out["sort_name"] = artist->sort_name;

  repo::AlbumRepo albumRepo(databasePath());
  auto albums = albumRepo.byArtist(id);
  int i = 0;
  for (const auto &a : albums) {
    out["albums"][i]["id"] = a.id;
    out["albums"][i]["name"] = a.name;
    out["albums"][i]["album_artists"] = intList(a.artist_ids);
    out["albums"][i]["year"] = a.year;
    out["albums"][i]["cover_path"] = a.cover_path;
    ++i;
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

  repo::TrackRepo repo(databasePath());
  auto tracks = repo.recent(after_id, pegcount);

  crow::json::wvalue out(crow::json::type::List);
  int i = 0;
  for (const auto &t : tracks) {
    out[i] = trackToJson(t);
    ++i;
  }
  return out;
}

std::optional<crow::json::wvalue> Service::tag(const crow::json::rvalue &json) {
  if (!json.has("tag")) {
    return crow::json::wvalue(crow::json::type::List);
  }
  const std::string &tag_name = json["tag"].s();

  SQLite::Database &conn = db::conn(databasePath());
  SQLite::Statement genre_query(conn, schema::sql::kGenreByName);
  genre_query.bind(1, tag_name);
  if (!genre_query.executeStep()) {
    return crow::json::wvalue(crow::json::type::List);
  }
  const int genre_id = genre_query.getColumn(0).getInt();

  repo::TrackRepo repo(databasePath());
  auto tracks = repo.byGenre(genre_id, config::get().page_size);

  crow::json::wvalue out(crow::json::type::List);
  int i = 0;
  for (const auto &t : tracks) {
    out[i] = trackToJson(t);
    ++i;
  }
  return out;
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
  const std::string &query = json["q"].s();

  repo::TrackRepo repo(databasePath());
  auto tracks = repo.search(query, config::get().page_size);

  crow::json::wvalue out(crow::json::type::List);
  int i = 0;
  for (const auto &t : tracks) {
    out[i] = trackToJson(t);
    ++i;
  }
  return out;
}
