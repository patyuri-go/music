#include "database.h"

#include "SQLiteCpp/Column.h"
#include "SQLiteCpp/Database.h"
#include "SQLiteCpp/Statement.h"
#include "config.h"
#include "db.h"
#include "schema.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace repo {
namespace {

std::string getText(const SQLite::Statement &q, int col) {
  if (q.getColumn(col).isNull()) {
    return {};
  }
  return q.getColumn(col).getText();
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

Track readTrack(SQLite::Statement &q) {
  Track t;
  t.id = q.getColumn(0).getInt();
  t.root_id = q.getColumn(1).getInt();
  t.path = getText(q, 2);
  t.title = getText(q, 3);
  t.album_id = q.getColumn(4).getInt();
  t.disc_no = q.getColumn(5).getInt();
  t.track_no = q.getColumn(6).getInt();
  t.duration_ms = q.getColumn(7).getInt();
  t.bitrate = q.getColumn(8).getInt();
  t.sample_rate = q.getColumn(9).getInt();
  t.channels = q.getColumn(10).getInt();
  t.format = getText(q, 11);
  t.size_bytes = q.getColumn(12).getInt64();
  t.mtime = q.getColumn(13).getInt64();
  t.added_at = getText(q, 14);
  t.play_count = q.getColumn(15).getInt();
  t.last_played = getText(q, 16);
  t.rating = q.getColumn(17).getInt();
  t.artist_ids = parseIntList(getText(q, 18));
  return t;
}

Album readAlbum(SQLite::Statement &q) {
  Album a;
  a.id = q.getColumn(0).getInt();
  a.name = getText(q, 1);
  a.year = q.getColumn(2).getInt();
  a.cover_path = getText(q, 3);
  a.artist_ids = parseIntList(getText(q, 5));
  return a;
}

Artist readArtist(SQLite::Statement &q) {
  Artist a;
  a.id = q.getColumn(0).getInt();
  a.name = getText(q, 1);
  a.sort_name = getText(q, 2);
  return a;
}

ScanRoot readScanRoot(SQLite::Statement &q) {
  ScanRoot sr;
  sr.id = q.getColumn(0).getInt();
  sr.path = getText(q, 1);
  sr.added_at = getText(q, 2);
  sr.last_scan = getText(q, 3);
  sr.user = getText(q, 4);
  sr.pass = getText(q, 5);
  sr.name = getText(q, 6);
  sr.type = getText(q, 7);
  return sr;
}

} // namespace

// ScanRootRepo

ScanRootRepo::ScanRootRepo(const std::string &path)  : pdb_(db::conn(path)) {}

std::vector<ScanRoot> ScanRootRepo::list() {
  std::vector<ScanRoot> result;
  SQLite::Statement q(pdb_, schema::sql::kScanRoots);
  while (q.executeStep()) {
    result.push_back(readScanRoot(q));
  }
  return result;
}

int ScanRootRepo::add(const std::string &path, const std::string &user,
                      const std::string &pass, const std::string &name,
                      const std::string &type) {
  SQLite::Statement check(pdb_, schema::sql::kScanRootByPath);
  check.bind(1, path);
  if (check.executeStep()) {
    return check.getColumn(0).getInt();
  }
  SQLite::Statement insert(pdb_, schema::sql::kScanRootInsert);
  insert.bind(1, path);
  insert.bind(2, user);
  insert.bind(3, pass);
  insert.bind(4, name);
  insert.bind(5, type);
  insert.executeStep();
  return static_cast<int>(pdb_.getLastInsertRowid());
}

void ScanRootRepo::updateName(int id, const std::string &name) {
  SQLite::Statement q(pdb_, schema::sql::kScanRootUpdateName);
  q.bind(1, name);
  q.bind(2, id);
  q.executeStep();
}

bool ScanRootRepo::remove(int id) {
  SQLite::Statement q(pdb_, schema::sql::kScanRootDelete);
  q.bind(1, id);
  return q.executeStep() || q.getChanges() > 0;
}

std::optional<ScanRoot> ScanRootRepo::byId(int id) {
  SQLite::Statement q(pdb_, schema::sql::kScanRootById);
  q.bind(1, id);
  if (!q.executeStep()) {
    return std::nullopt;
  }
  return readScanRoot(q);
}

std::optional<ScanRoot> ScanRootRepo::byName(const std::string &name) {
  SQLite::Statement q(pdb_, schema::sql::kScanRootByName);
  q.bind(1, name);
  if (!q.executeStep()) {
    return std::nullopt;
  }
  return readScanRoot(q);
}

// TrackRepo

TrackRepo::TrackRepo(const std::string &path)  : pdb_(db::conn(path)) {}

std::optional<Track> TrackRepo::byId(int id) {
  SQLite::Statement q(pdb_, schema::sql::kTrackById);
  q.bind(1, id);
  if (!q.executeStep()) {
    return std::nullopt;
  }
  return readTrack(q);
}

std::vector<Track> TrackRepo::byAlbum(int album_id) {
  std::vector<Track> result;
  SQLite::Statement q(pdb_, schema::sql::kTracksByAlbum);
  q.bind(1, album_id);
  while (q.executeStep()) {
    result.push_back(readTrack(q));
  }
  return result;
}

std::vector<Track> TrackRepo::byRootId(int root_id) {
  std::vector<Track> result;
  SQLite::Statement q(pdb_, schema::sql::kTrackByRootId);
  q.bind(1, root_id);
  while (q.executeStep()) {
    result.push_back(readTrack(q));
  }
  return result;
}

std::vector<Track> TrackRepo::recent(int after_id, int limit) {
  std::vector<Track> result;
  SQLite::Statement q(pdb_, schema::sql::kTracksRecent);
  q.bind(1, after_id);
  q.bind(2, limit);
  while (q.executeStep()) {
    result.push_back(readTrack(q));
  }
  return result;
}

std::vector<Track> TrackRepo::byGenre(int genre_id, int limit) {
  std::vector<Track> result;
  SQLite::Statement q(pdb_, schema::sql::kTracksByGenre);
  q.bind(1, genre_id);
  q.bind(2, limit);
  while (q.executeStep()) {
    result.push_back(readTrack(q));
  }
  return result;
}

std::vector<Track> TrackRepo::search(const std::string &query, int limit) {
  std::vector<Track> result;
  SQLite::Statement q(pdb_, schema::sql::kSearchByTitle);
  const std::string pattern = "%" + query + "%";
  q.bind(1, pattern);
  q.bind(2, limit);
  while (q.executeStep()) {
    result.push_back(readTrack(q));
  }
  return result;
}

std::vector<Track> TrackRepo::recentPlays(int limit) {
  std::vector<Track> result;
  SQLite::Statement q(pdb_, schema::sql::kRecentPlays);
  q.bind(1, limit);
  while (q.executeStep()) {
    result.push_back(readTrack(q));
  }
  return result;
}

bool TrackRepo::recordPlay(int track_id) {
  SQLite::Statement event(pdb_, "INSERT INTO play_event(track_id, played_at, seconds_played) VALUES (?, datetime('now','localtime'), 0)");
  event.bind(1, track_id);
  event.executeStep();

  SQLite::Statement stats(pdb_, schema::sql::kTrackUpdateStats);
  stats.bind(1, track_id);
  return stats.executeStep() || stats.getChanges() > 0;
}

std::vector<int> TrackRepo::genresByTrackId(int track_id) {
  std::vector<int> result;
  SQLite::Statement q(pdb_, schema::sql::kTrackGenresByTrackId);
  q.bind(1, track_id);
  while (q.executeStep()) {
    result.push_back(q.getColumn(0).getInt());
  }
  return result;
}

std::vector<int> TrackRepo::artistIdsByTrackId(int track_id) {
  std::vector<int> result;
  SQLite::Statement q(pdb_, schema::sql::kTrackArtistsByTrackId);
  q.bind(1, track_id);
  while (q.executeStep()) {
    result.push_back(q.getColumn(0).getInt());
  }
  return result;
}

std::vector<Track> TrackRepo::byArtist(int artist_id, int limit) {
  std::vector<Track> result;
  SQLite::Statement q(pdb_, schema::sql::kTracksByArtist);
  q.bind(1, artist_id);
  q.bind(2, limit);
  while (q.executeStep()) {
    result.push_back(readTrack(q));
  }
  return result;
}

bool TrackRepo::updatePartial(const Track &t) {
  SQLite::Statement q(pdb_, schema::sql::kTrackUpdatePartial);
  q.bind(1, t.title);
  q.bind(2, t.album_id);
  q.bind(3, t.disc_no);
  q.bind(4, t.track_no);
  q.bind(5, t.duration_ms);
  q.bind(6, t.bitrate);
  q.bind(7, t.sample_rate);
  q.bind(8, t.channels);
  q.bind(9, t.format);
  q.bind(10, static_cast<int64_t>(t.size_bytes));
  q.bind(11, static_cast<int64_t>(t.mtime));
  q.bind(12, t.rating);
  q.bind(13, t.id);
  q.executeStep();
  return q.getChanges() > 0;
}

bool TrackRepo::remove(int id) {
  SQLite::Statement q(pdb_, schema::sql::kTrackDelete);
  q.bind(1, id);
  return q.executeStep() || q.getChanges() > 0;
}

// AlbumRepo

AlbumRepo::AlbumRepo(const std::string &path)  : pdb_(db::conn(path)) {}

std::optional<Album> AlbumRepo::byId(int id) {
  SQLite::Statement q(pdb_, schema::sql::kAlbumById);
  q.bind(1, id);
  if (!q.executeStep()) {
    return std::nullopt;
  }
  return readAlbum(q);
}

std::vector<Album> AlbumRepo::byArtist(int artist_id) {
  std::vector<Album> result;
  SQLite::Statement q(pdb_, schema::sql::kAlbumsByArtist);
  q.bind(1, artist_id);
  while (q.executeStep()) {
    result.push_back(readAlbum(q));
  }
  return result;
}

std::vector<Album> AlbumRepo::search(const std::string &query, int limit) {
  std::vector<Album> result;
  SQLite::Statement q(pdb_, schema::sql::kAlbumsSearchByName);
  const std::string pattern = "%" + query + "%";
  q.bind(1, pattern);
  q.bind(2, limit);
  while (q.executeStep()) {
    result.push_back(readAlbum(q));
  }
  return result;
}

std::vector<int> AlbumRepo::artistIdsByAlbumId(int album_id) {
  std::vector<int> result;
  SQLite::Statement q(pdb_, schema::sql::kAlbumArtistsByAlbumId);
  q.bind(1, album_id);
  while (q.executeStep()) {
    result.push_back(q.getColumn(0).getInt());
  }
  return result;
}

// ArtistRepo

ArtistRepo::ArtistRepo(const std::string &path)  : pdb_(db::conn(path)) {}

std::vector<Artist> ArtistRepo::search(const std::string &query, int limit) {
  std::vector<Artist> result;
  SQLite::Statement q(pdb_, schema::sql::kArtistsSearchByName);
  const std::string pattern = "%" + query + "%";
  q.bind(1, pattern);
  q.bind(2, limit);
  while (q.executeStep()) {
    result.push_back(readArtist(q));
  }
  return result;
}

std::optional<Artist> ArtistRepo::byId(int id) {
  SQLite::Statement q(pdb_, schema::sql::kArtistById);
  q.bind(1, id);
  if (!q.executeStep()) {
    return std::nullopt;
  }
  return readArtist(q);
}

} // namespace repo
