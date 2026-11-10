#include "database.h"

#include "db.h"
#include "schema.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace repo {
namespace {

std::string asText(const db::Cell &cell) {
  if (const auto *value = std::get_if<std::string>(&cell.value)) {
    return *value;
  }
  if (const auto *value = std::get_if<std::int64_t>(&cell.value)) {
    return std::to_string(*value);
  }
  if (const auto *value = std::get_if<double>(&cell.value)) {
    return std::to_string(*value);
  }
  return {};
}

std::int64_t asInt(const db::Cell &cell) {
  if (const auto *value = std::get_if<std::int64_t>(&cell.value)) {
    return *value;
  }
  if (const auto *value = std::get_if<double>(&cell.value)) {
    return static_cast<std::int64_t>(*value);
  }
  if (const auto *value = std::get_if<std::string>(&cell.value)) {
    try {
      return std::stoll(*value);
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

const db::Cell &cellAt(const db::Row &row, std::size_t index) {
  static const db::Cell kNull{db::Null{}};
  if (index >= row.size()) {
    return kNull;
  }
  return row[index];
}

std::vector<int> parseIntList(const std::string &csv) {
  std::vector<int> out;
  std::size_t start = 0;
  while (start < csv.size()) {
    const std::size_t comma = csv.find(',', start);
    const std::string part =
        (comma == std::string::npos) ? csv.substr(start)
                                     : csv.substr(start, comma - start);
    if (!part.empty()) {
      out.push_back(std::stoi(part));
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

Track readTrack(const db::Row &row) {
  Track t;
  t.id = static_cast<int>(asInt(cellAt(row, 0)));
  t.root_id = static_cast<int>(asInt(cellAt(row, 1)));
  t.path = asText(cellAt(row, 2));
  t.title = asText(cellAt(row, 3));
  t.album_id = static_cast<int>(asInt(cellAt(row, 4)));
  t.disc_no = static_cast<int>(asInt(cellAt(row, 5)));
  t.track_no = static_cast<int>(asInt(cellAt(row, 6)));
  t.duration_ms = static_cast<int>(asInt(cellAt(row, 7)));
  t.bitrate = static_cast<int>(asInt(cellAt(row, 8)));
  t.sample_rate = static_cast<int>(asInt(cellAt(row, 9)));
  t.channels = static_cast<int>(asInt(cellAt(row, 10)));
  t.format = asText(cellAt(row, 11));
  t.size_bytes = static_cast<long long>(asInt(cellAt(row, 12)));
  t.mtime = static_cast<long long>(asInt(cellAt(row, 13)));
  t.added_at = asText(cellAt(row, 14));
  t.play_count = static_cast<int>(asInt(cellAt(row, 15)));
  t.last_played = asText(cellAt(row, 16));
  t.rating = static_cast<int>(asInt(cellAt(row, 17)));
  t.artist_ids = parseIntList(asText(cellAt(row, 18)));
  return t;
}

Album readAlbum(const db::Row &row) {
  Album a;
  a.id = static_cast<int>(asInt(cellAt(row, 0)));
  a.name = asText(cellAt(row, 1));
  a.year = static_cast<int>(asInt(cellAt(row, 2)));
  a.cover_path = asText(cellAt(row, 3));
  a.artist_ids = parseIntList(asText(cellAt(row, 5)));
  return a;
}

Artist readArtist(const db::Row &row) {
  Artist a;
  a.id = static_cast<int>(asInt(cellAt(row, 0)));
  a.name = asText(cellAt(row, 1));
  a.sort_name = asText(cellAt(row, 2));
  return a;
}

ScanRoot readScanRoot(const db::Row &row) {
  ScanRoot sr;
  sr.id = static_cast<int>(asInt(cellAt(row, 0)));
  sr.path = asText(cellAt(row, 1));
  sr.added_at = asText(cellAt(row, 2));
  sr.last_scan = asText(cellAt(row, 3));
  sr.user = asText(cellAt(row, 4));
  sr.pass = asText(cellAt(row, 5));
  sr.name = asText(cellAt(row, 6));
  sr.type = asText(cellAt(row, 7));
  return sr;
}

std::vector<Track> readTracks(const db::ResultSet &result) {
  std::vector<Track> out;
  out.reserve(result.rows.size());
  for (const auto &row : result.rows) {
    out.push_back(readTrack(row));
  }
  return out;
}

std::vector<Album> readAlbums(const db::ResultSet &result) {
  std::vector<Album> out;
  out.reserve(result.rows.size());
  for (const auto &row : result.rows) {
    out.push_back(readAlbum(row));
  }
  return out;
}

} // namespace

// ScanRootRepo

ScanRootRepo::ScanRootRepo(db::Session &session) : pdb_(session) {}

std::vector<ScanRoot> ScanRootRepo::list() {
  const auto result = pdb_.query(schema::sql::kScanRoots);
  std::vector<ScanRoot> out;
  out.reserve(result.rows.size());
  for (const auto &row : result.rows) {
    out.push_back(readScanRoot(row));
  }
  return out;
}

int ScanRootRepo::add(const std::string &path, const std::string &user,
                      const std::string &pass, const std::string &name,
                      const std::string &type) {
  const auto existing = pdb_.query(schema::sql::kScanRootByPath, {path});
  if (!existing.rows.empty()) {
    return static_cast<int>(asInt(existing.rows.front().front()));
  }
  pdb_.execute(schema::sql::kScanRootInsert, {path, user, pass, name, type});
  return static_cast<int>(pdb_.lastInsertId());
}

void ScanRootRepo::updateName(int id, const std::string &name) {
  pdb_.execute(schema::sql::kScanRootUpdateName,
               {name, static_cast<std::int64_t>(id)});
}

bool ScanRootRepo::remove(int id) {
  return pdb_.execute(schema::sql::kScanRootDelete,
                      {static_cast<std::int64_t>(id)}) > 0;
}

std::optional<ScanRoot> ScanRootRepo::byId(int id) {
  const auto result =
      pdb_.query(schema::sql::kScanRootById, {static_cast<std::int64_t>(id)});
  if (result.rows.empty()) {
    return std::nullopt;
  }
  return readScanRoot(result.rows.front());
}

std::optional<ScanRoot> ScanRootRepo::byName(const std::string &name) {
  const auto result = pdb_.query(schema::sql::kScanRootByName, {name});
  if (result.rows.empty()) {
    return std::nullopt;
  }
  return readScanRoot(result.rows.front());
}

// TrackRepo

TrackRepo::TrackRepo(db::Session &session) : pdb_(session) {}

std::optional<Track> TrackRepo::byId(int id) {
  const auto result = pdb_.query(schema::sql::trackById(pdb_.dialect()),
                                 {static_cast<std::int64_t>(id)});
  if (result.rows.empty()) {
    return std::nullopt;
  }
  return readTrack(result.rows.front());
}

std::vector<Track> TrackRepo::byAlbum(int album_id) {
  return readTracks(pdb_.query(schema::sql::tracksByAlbum(pdb_.dialect()),
                               {static_cast<std::int64_t>(album_id)}));
}

std::vector<Track> TrackRepo::byRootId(int root_id) {
  return readTracks(pdb_.query(schema::sql::trackByRootId(pdb_.dialect()),
                               {static_cast<std::int64_t>(root_id)}));
}

std::vector<Track> TrackRepo::recent(int after_id, int limit) {
  return readTracks(
      pdb_.query(schema::sql::tracksRecent(pdb_.dialect()),
                 {static_cast<std::int64_t>(after_id),
                  static_cast<std::int64_t>(limit)}));
}

std::vector<Track> TrackRepo::byGenre(int genre_id, int limit) {
  return readTracks(
      pdb_.query(schema::sql::tracksByGenre(pdb_.dialect()),
                 {static_cast<std::int64_t>(genre_id),
                  static_cast<std::int64_t>(limit)}));
}

std::vector<Track> TrackRepo::search(const std::string &query, int limit) {
  const std::string pattern = "%" + query + "%";
  return readTracks(
      pdb_.query(schema::sql::searchByTitle(pdb_.dialect()),
                 {pattern, static_cast<std::int64_t>(limit)}));
}

std::vector<Track> TrackRepo::recentPlays(int limit) {
  return readTracks(pdb_.query(schema::sql::recentPlays(pdb_.dialect()),
                               {static_cast<std::int64_t>(limit)}));
}

bool TrackRepo::recordPlay(int track_id) {
  const std::string insert =
      "INSERT INTO play_event(track_id, played_at, seconds_played) "
      "VALUES (?, " +
      schema::sql::nowExpr(pdb_.dialect()) + ", 0)";
  pdb_.execute(insert, {static_cast<std::int64_t>(track_id)});
  return pdb_.execute(schema::sql::trackUpdateStats(pdb_.dialect()),
                      {static_cast<std::int64_t>(track_id)}) > 0;
}

std::vector<int> TrackRepo::genresByTrackId(int track_id) {
  const auto result = pdb_.query(schema::sql::kTrackGenresByTrackId,
                                 {static_cast<std::int64_t>(track_id)});
  std::vector<int> out;
  out.reserve(result.rows.size());
  for (const auto &row : result.rows) {
    out.push_back(static_cast<int>(asInt(row.front())));
  }
  return out;
}

std::vector<int> TrackRepo::artistIdsByTrackId(int track_id) {
  const auto result = pdb_.query(schema::sql::kTrackArtistsByTrackId,
                                 {static_cast<std::int64_t>(track_id)});
  std::vector<int> out;
  out.reserve(result.rows.size());
  for (const auto &row : result.rows) {
    out.push_back(static_cast<int>(asInt(row.front())));
  }
  return out;
}

std::vector<Track> TrackRepo::byArtist(int artist_id, int limit) {
  return readTracks(
      pdb_.query(schema::sql::tracksByArtist(pdb_.dialect()),
                 {static_cast<std::int64_t>(artist_id),
                  static_cast<std::int64_t>(limit)}));
}

bool TrackRepo::updatePartial(const Track &t) {
  return pdb_.execute(schema::sql::kTrackUpdatePartial,
                      {t.title,
                       static_cast<std::int64_t>(t.album_id),
                       static_cast<std::int64_t>(t.disc_no),
                       static_cast<std::int64_t>(t.track_no),
                       static_cast<std::int64_t>(t.duration_ms),
                       static_cast<std::int64_t>(t.bitrate),
                       static_cast<std::int64_t>(t.sample_rate),
                       static_cast<std::int64_t>(t.channels),
                       t.format,
                       static_cast<std::int64_t>(t.size_bytes),
                       static_cast<std::int64_t>(t.mtime),
                       static_cast<std::int64_t>(t.rating),
                       static_cast<std::int64_t>(t.id)}) > 0;
}

bool TrackRepo::remove(int id) {
  return pdb_.execute(schema::sql::kTrackDelete,
                      {static_cast<std::int64_t>(id)}) > 0;
}

// AlbumRepo

AlbumRepo::AlbumRepo(db::Session &session) : pdb_(session) {}

std::optional<Album> AlbumRepo::byId(int id) {
  const auto result = pdb_.query(schema::sql::albumById(pdb_.dialect()),
                                 {static_cast<std::int64_t>(id)});
  if (result.rows.empty()) {
    return std::nullopt;
  }
  return readAlbum(result.rows.front());
}

std::vector<Album> AlbumRepo::byArtist(int artist_id) {
  return readAlbums(pdb_.query(schema::sql::albumsByArtist(pdb_.dialect()),
                               {static_cast<std::int64_t>(artist_id)}));
}

std::vector<Album> AlbumRepo::search(const std::string &query, int limit) {
  const std::string pattern = "%" + query + "%";
  return readAlbums(
      pdb_.query(schema::sql::albumsSearchByName(pdb_.dialect()),
                 {pattern, static_cast<std::int64_t>(limit)}));
}

std::vector<int> AlbumRepo::artistIdsByAlbumId(int album_id) {
  const auto result = pdb_.query(schema::sql::kAlbumArtistsByAlbumId,
                                 {static_cast<std::int64_t>(album_id)});
  std::vector<int> out;
  out.reserve(result.rows.size());
  for (const auto &row : result.rows) {
    out.push_back(static_cast<int>(asInt(row.front())));
  }
  return out;
}

// ArtistRepo

ArtistRepo::ArtistRepo(db::Session &session) : pdb_(session) {}

std::vector<Artist> ArtistRepo::search(const std::string &query, int limit) {
  const std::string pattern = "%" + query + "%";
  const auto result =
      pdb_.query(schema::sql::artistsSearchByName(pdb_.dialect()),
                 {pattern, static_cast<std::int64_t>(limit)});
  std::vector<Artist> out;
  out.reserve(result.rows.size());
  for (const auto &row : result.rows) {
    out.push_back(readArtist(row));
  }
  return out;
}

std::optional<Artist> ArtistRepo::byId(int id) {
  const auto result = pdb_.query(schema::sql::kArtistById,
                                 {static_cast<std::int64_t>(id)});
  if (result.rows.empty()) {
    return std::nullopt;
  }
  return readArtist(result.rows.front());
}

} // namespace repo
