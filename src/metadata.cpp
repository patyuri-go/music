#include "metadata.h"

#include <filesystem>

#include <taglib/fileref.h>
#include <taglib/audioproperties.h>
#include <taglib/tag.h>
#include <taglib/toolkit/tpropertymap.h>

#include <chrono>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <optional>
#include <vector>

#include "api/webdav/webdav.h"
#include "api/webdav/webdav_stream.h"

namespace metadata {

std::vector<std::string> splitArtists(const std::string &value) {
  std::vector<std::string> out;
  const auto addName = [&out](const std::string &raw) {
    const std::size_t b = raw.find_first_not_of(" \t\r\n");
    const std::size_t e = raw.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) {
      return;
    }
    const std::string s = raw.substr(b, e - b + 1);
    for (const auto &x : out) {
      if (x == s) {
        return;
      }
    }
    out.push_back(s);
  };
  // A credit string may contain role prefixes and multiple names, e.g.
  //   "Compose: typeMARS + GhostFinal | Arrange: typeMARS | Vocals: X"
  // Split into role segments on top-level separators, drop an ASCII "Role:"
  // prefix from each segment, then split the remaining names on '+'.
  std::vector<std::string> segments;
  std::string cur;
  for (const char c : value) {
    if (c == ';' || c == '/' || c == '|') {
      segments.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  segments.push_back(cur);
  for (std::string &segment : segments) {
    const std::size_t colon = segment.find(':');
    if (colon != std::string::npos && colon > 0) {
      bool is_role = true;
      for (std::size_t i = 0; i < colon; ++i) {
        const unsigned char c = static_cast<unsigned char>(segment[i]);
        if (!(std::isalpha(c) || c == ' ' || c == '\t')) {
          is_role = false;
          break;
        }
      }
      if (is_role) {
        segment = segment.substr(colon + 1);
      }
    }
    std::string name;
    for (const char c : segment) {
      if (c == '+') {
        addName(name);
        name.clear();
      } else {
        name.push_back(c);
      }
    }
    addName(name);
  }
  return out;
}

namespace {

std::vector<std::string> readAlbumArtists(const TagLib::FileRef &fileRef) {
  std::vector<std::string> out;
  const TagLib::PropertyMap props = fileRef.properties();
  const auto it = props.find(TagLib::String("ALBUMARTIST"));
  if (it != props.end()) {
    for (const auto &value : it->second) {
      for (auto &name : splitArtists(value.to8Bit(true))) {
        out.push_back(name);
      }
    }
  }
  return out;
}

std::string formatFromPath(const std::string &path) {
  const auto pos = path.rfind('.');
  if (pos == std::string::npos) {
    return {};
  }
  return path.substr(pos + 1);
}

std::string replaceExtension(const std::string &path,
                             const std::string &ext) {
  const auto slash = path.find_last_of('/');
  const auto dot = path.rfind('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
    return path + ext;
  }
  return path.substr(0, dot) + ext;
}

std::string readEmbeddedLyrics(const TagLib::FileRef &fileRef) {
  const TagLib::PropertyMap props = fileRef.properties();
  for (const char *key : {"LYRICS", "UNSYNCEDLYRICS", "LYRIC"}) {
    const auto it = props.find(TagLib::String(key));
    if (it != props.end() && !it->second.isEmpty()) {
      const std::string text = it->second.front().to8Bit(true);
      if (!text.empty()) {
        return text;
      }
    }
  }
  return {};
}

std::string readLocalTextFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

TrackMeta buildTrackMeta(TagLib::FileRef &fileRef, const std::string &path) {
  const TagLib::Tag *tag = fileRef.tag();
  const TagLib::AudioProperties *props = fileRef.audioProperties();

  TrackMeta meta;
  meta.title = tag->title().to8Bit(true);
  meta.artist = tag->artist().to8Bit(true);
  meta.artists = splitArtists(meta.artist);
  meta.album = tag->album().to8Bit(true);
  meta.album_artists = readAlbumArtists(fileRef);
  if (meta.album_artists.empty()) {
    meta.album_artists = meta.artists;
  }
  meta.year = tag->year();
  meta.genre = tag->genre().to8Bit(true);
  meta.track_no = tag->track();
  meta.disc_no = 1;
  {
    const TagLib::PropertyMap props = fileRef.properties();
    const auto it = props.find(TagLib::String("DISCNUMBER"));
    if (it != props.end() && !it->second.isEmpty()) {
      const int disc = it->second.front().toInt();
      if (disc > 0) {
        meta.disc_no = disc;
      }
    }
  }

  if (props) {
    meta.duration_ms = props->lengthInMilliseconds();
    meta.bitrate = props->bitrate();
    meta.sample_rate = props->sampleRate();
    meta.channels = props->channels();
  }

  meta.format = formatFromPath(path);
  meta.lyrics = readEmbeddedLyrics(fileRef);
  return meta;
}

FileMeta buildFileMeta(TagLib::FileRef &fileRef) {
  const TagLib::Tag *tag = fileRef.tag();
  FileMeta meta;
  meta.title = tag->title().to8Bit(true);
  meta.artist = tag->artist().to8Bit(true);
  meta.artists = splitArtists(meta.artist);
  meta.album = tag->album().to8Bit(true);
  meta.album_artists = readAlbumArtists(fileRef);
  if (meta.album_artists.empty()) {
    meta.album_artists = meta.artists;
  }
  meta.genre = tag->genre().to8Bit(true);
  meta.year = tag->year();
  meta.comment = tag->comment().to8Bit(true);
  meta.track_no = tag->track();
  meta.disc_no = 1;
  {
    const TagLib::PropertyMap props = fileRef.properties();
    const auto it = props.find(TagLib::String("DISCNUMBER"));
    if (it != props.end() && !it->second.isEmpty()) {
      const int disc = it->second.front().toInt();
      if (disc > 0) {
        meta.disc_no = disc;
      }
    }
  }
  const TagLib::AudioProperties *props = fileRef.audioProperties();
  if (props) {
    meta.bitrate = props->bitrate();
    meta.sample_rate = props->sampleRate();
    meta.channels = props->channels();
  }
  meta.lyrics = readEmbeddedLyrics(fileRef);
  return meta;
}

void applyFileMeta(TagLib::FileRef &fileRef, const FileMeta &meta) {
  TagLib::Tag *tag = fileRef.tag();
  if (!tag) {
    return;
  }
  tag->setTitle(TagLib::String(meta.title, TagLib::String::UTF8));
  if (!meta.artists.empty()) {
    std::string joined;
    for (std::size_t i = 0; i < meta.artists.size(); ++i) {
      if (i != 0) {
        joined += "; ";
      }
      joined += meta.artists[i];
    }
    tag->setArtist(TagLib::String(joined, TagLib::String::UTF8));
  } else {
    tag->setArtist(TagLib::String(meta.artist, TagLib::String::UTF8));
  }
  tag->setAlbum(TagLib::String(meta.album, TagLib::String::UTF8));
  tag->setGenre(TagLib::String(meta.genre, TagLib::String::UTF8));
  tag->setYear(meta.year);
  tag->setComment(TagLib::String(meta.comment, TagLib::String::UTF8));
  tag->setTrack(meta.track_no > 0 ? static_cast<unsigned int>(meta.track_no) : 0u);

  // Tag has no setter for the disc number; write it through the property map.
  TagLib::File *file = fileRef.file();
  if (file) {
    TagLib::PropertyMap props = file->properties();
    if (meta.disc_no > 0) {
      props.replace("DISCNUMBER", TagLib::String::number(meta.disc_no));
    } else {
      props.erase("DISCNUMBER");
    }
    file->setProperties(props);
  }
}

} // namespace

std::optional<TrackMeta> read(const std::string &path) {
  if (webdav::isWebdavUrl(path)) {
    WebdavStream stream(path, true);
    if (!stream.isOpen()) {
      return std::nullopt;
    }
    TagLib::FileRef fileRef(&stream, true, TagLib::AudioProperties::Average);
    if (fileRef.isNull() || !fileRef.tag()) {
      return std::nullopt;
    }
    return buildTrackMeta(fileRef, path);
  }

  TagLib::FileRef fileRef(path.c_str());
  if (fileRef.isNull() || !fileRef.tag()) {
    return std::nullopt;
  }

  TrackMeta meta = buildTrackMeta(fileRef, path);

  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (!ec) {
    meta.size_bytes = size;
  }

  const auto ftime = std::filesystem::last_write_time(path, ec);
  if (!ec) {
    using namespace std::chrono;
    const auto sctp = time_point_cast<seconds>(ftime);
    meta.mtime = static_cast<long long>(sctp.time_since_epoch().count());
  }

  return meta;
}

std::optional<FileMeta> readFileMeta(const std::string &path) {
  if (webdav::isWebdavUrl(path)) {
    WebdavStream stream(path, true);
    if (!stream.isOpen()) {
      return std::nullopt;
    }
    TagLib::FileRef fileRef(&stream, true, TagLib::AudioProperties::Average);
    if (fileRef.isNull() || !fileRef.tag()) {
      return std::nullopt;
    }
    FileMeta meta = buildFileMeta(fileRef);
    meta.format = formatFromPath(path);
    const long long len = stream.length();
    if (len > 0) {
      meta.size_bytes = len;
    }
    return meta;
  }

  TagLib::FileRef fileRef(path.c_str());
  if (fileRef.isNull() || !fileRef.tag()) {
    return std::nullopt;
  }
  FileMeta meta = buildFileMeta(fileRef);
  meta.format = formatFromPath(path);
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (!ec) {
    meta.size_bytes = size;
  }
  return meta;
}

bool writeFileMeta(const std::string &path, const FileMeta &meta) {
  if (webdav::isWebdavUrl(path)) {
    WebdavStream stream(path, false);
    if (!stream.isOpen()) {
      return false;
    }
    {
      TagLib::FileRef fileRef(&stream, false);
      if (fileRef.isNull() || !fileRef.tag()) {
        return false;
      }
      applyFileMeta(fileRef, meta);
      if (!fileRef.save()) {
        return false;
      }
    }
    return stream.flush();
  }

  TagLib::FileRef fileRef(path.c_str());
  if (fileRef.isNull() || !fileRef.tag()) {
    return false;
  }
  applyFileMeta(fileRef, meta);
  return fileRef.save();
}

std::string readSiblingLyrics(const std::string &path) {
  const std::string lrc = replaceExtension(path, ".lrc");
  if (webdav::isWebdavUrl(path)) {
    webdav::Url u = webdav::resolveCredentials(lrc);
    webdav::Client client(u);
    webdav::Response r = client.getAll(u.path);
    if (r.status == 200) {
      return r.body;
    }
    return {};
  }
  return readLocalTextFile(lrc);
}

bool isSyncedLyrics(const std::string &text) {
  // LRC 时间标签形如 [mm:ss]、[mm:ss.xx] 或 [mm:ss:xx]。
  for (std::size_t i = 0; i + 2 < text.size(); ++i) {
    if (text[i] != '[') {
      continue;
    }
    std::size_t j = i + 1;
    if (!std::isdigit(static_cast<unsigned char>(text[j]))) {
      continue;
    }
    std::size_t digits = 0;
    while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) {
      ++j;
      ++digits;
    }
    if (digits > 0 && j < text.size() && text[j] == ':') {
      return true;
    }
  }
  return false;
}

} // namespace metadata
