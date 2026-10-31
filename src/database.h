#ifndef DATABASE_H
#define DATABASE_H

#include <optional>
#include <string>
#include <vector>

namespace SQLite {
class Database;
} // namespace SQLite

namespace repo {

struct Track {
  int id = 0;
  int root_id = 0;
  std::string path;
  std::string title;
  std::vector<int> artist_ids;
  int album_id = 0;
  int disc_no = 1;
  int track_no = 0;
  int duration_ms = 0;
  int bitrate = 0;
  int sample_rate = 0;
  int channels = 0;
  std::string format;
  long long size_bytes = 0;
  long long mtime = 0;
  std::string added_at;
  int play_count = 0;
  std::string last_played;
  int rating = 0;
};

struct Album {
  int id = 0;
  std::string name;
  std::vector<int> artist_ids;
  int year = 0;
  std::string cover_path;
};

struct Artist {
  int id = 0;
  std::string name;
  std::string sort_name;
};

struct ScanRoot {
  int id = 0;
  std::string path;
  std::string added_at;
  std::string last_scan;
  std::string user;
  std::string pass;
  std::string name;
  std::string type;
};

class ScanRootRepo {
public:
  explicit ScanRootRepo(const std::string &path);
  std::vector<ScanRoot> list();
  std::optional<ScanRoot> byId(int id);
  std::optional<ScanRoot> byName(const std::string &name);
  int add(const std::string &path, const std::string &user = "",
          const std::string &pass = "", const std::string &name = "",
          const std::string &type = "");
  void updateName(int id, const std::string &name);
  bool remove(int id);

private:
  SQLite::Database &pdb_;
};

class TrackRepo {
public:
  explicit TrackRepo(const std::string &path);
  std::optional<Track> byId(int id);
  std::vector<Track> byAlbum(int album_id);
  std::vector<Track> byArtist(int artist_id, int limit);
  std::vector<Track> byRootId(int root_id);
  std::vector<Track> recent(int after_id, int limit);
  std::vector<Track> byGenre(int genre_id, int limit);
  std::vector<Track> search(const std::string &query, int limit);
  std::vector<Track> recentPlays(int limit);
  std::vector<int> genresByTrackId(int track_id);
  std::vector<int> artistIdsByTrackId(int track_id);
  bool recordPlay(int track_id);
  bool updatePartial(const Track &t);
  bool remove(int id);

private:
  SQLite::Database &pdb_;
};

class AlbumRepo {
public:
  explicit AlbumRepo(const std::string &path);
  std::optional<Album> byId(int id);
  std::vector<Album> byArtist(int artist_id);
  // 专辑名包含匹配，供 Searcher 出 Album 命中。
  std::vector<Album> search(const std::string &query, int limit);
  std::vector<int> artistIdsByAlbumId(int album_id);

private:
  SQLite::Database &pdb_;
};

class ArtistRepo {
public:
  explicit ArtistRepo(const std::string &path);
  std::optional<Artist> byId(int id);
  // 艺术家名包含匹配，供 Searcher 出 Artist 命中。
  std::vector<Artist> search(const std::string &query, int limit);

private:
  SQLite::Database &pdb_;
};

} // namespace repo

#endif
