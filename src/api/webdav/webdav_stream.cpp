#include "webdav_stream.h"

#include <algorithm>
#include <cstddef>

namespace {
constexpr long long kChunk = 128 * 1024;
} // namespace

WebdavStream::WebdavStream(const std::string &url, bool read_only)
    : url_(url), read_only_(read_only) {
  webdav::Url u = webdav::resolveCredentials(url);
  path_ = u.path.empty() ? "/" : u.path;
  client_ = std::make_unique<webdav::Client>(u);

  if (read_only_) {
    open_ = true;
  } else {
    webdav::Response r = client_->getAll(path_);
    if (r.status == 200) {
      data_ = std::move(r.body);
      length_ = static_cast<TagLib::offset_t>(data_.size());
      open_ = true;
    }
  }
}

WebdavStream::~WebdavStream() = default;

TagLib::FileName WebdavStream::name() const { return url_.c_str(); }

TagLib::offset_t WebdavStream::length() {
  if (length_ < 0) {
    length_ = client_->contentLength(path_);
    if (length_ < 0) {
      length_ = 0;
    }
  }
  return length_;
}

std::string WebdavStream::fetch(TagLib::offset_t offset, std::size_t count) {
  if (count == 0) {
    return {};
  }

  if (!read_only_) {
    if (offset < 0 || offset >= static_cast<TagLib::offset_t>(data_.size())) {
      return {};
    }
    const std::size_t end =
        std::min<std::size_t>(static_cast<std::size_t>(offset) + count,
                              data_.size());
    return data_.substr(static_cast<std::size_t>(offset),
                        end - static_cast<std::size_t>(offset));
  }

  if (cache_valid_ && offset >= cache_off_ &&
      offset + static_cast<TagLib::offset_t>(count) <=
          cache_off_ + static_cast<TagLib::offset_t>(cache_.size())) {
    return cache_.substr(static_cast<std::size_t>(offset - cache_off_), count);
  }

  const TagLib::offset_t total = length();
  if (total >= 0 && offset >= total) {
    return {};
  }
  long long want = std::max<long long>(static_cast<long long>(count), kChunk);
  if (total > 0 && offset + want > total) {
    want = total - offset;
  }
  if (want <= 0) {
    return {};
  }

  webdav::Response r = client_->getRange(path_, offset, want);
  if (r.status != 200 && r.status != 206) {
    return {};
  }
  cache_off_ = (r.status == 200) ? 0 : offset;
  cache_ = std::move(r.body);
  cache_valid_ = true;

  if (cache_.size() < static_cast<std::size_t>(offset - cache_off_)) {
    return {};
  }
  std::size_t avail =
      cache_.size() - static_cast<std::size_t>(offset - cache_off_);
  if (avail < count) {
    count = avail;
  }
  return cache_.substr(static_cast<std::size_t>(offset - cache_off_), count);
}

TagLib::ByteVector WebdavStream::readBlock(std::size_t length) {
  if (!open_ || length == 0) {
    return TagLib::ByteVector();
  }
  std::string bytes = fetch(pos_, length);
  pos_ += static_cast<TagLib::offset_t>(bytes.size());
  return TagLib::ByteVector(bytes.data(),
                            static_cast<unsigned int>(bytes.size()));
}

void WebdavStream::writeBlock(const TagLib::ByteVector &data) {
  if (read_only_) {
    return;
  }
  const std::size_t end = static_cast<std::size_t>(pos_) + data.size();
  if (data_.size() < end) {
    data_.resize(end, '\0');
  }
  std::copy(data.begin(), data.end(), data_.begin() + pos_);
  pos_ += static_cast<TagLib::offset_t>(data.size());
  length_ = static_cast<TagLib::offset_t>(data_.size());
}

void WebdavStream::insert(const TagLib::ByteVector &data,
                          TagLib::offset_t start, std::size_t replace) {
  if (read_only_) {
    return;
  }
  if (start < 0 || static_cast<std::size_t>(start) > data_.size()) {
    start = static_cast<TagLib::offset_t>(data_.size());
  }
  std::size_t s = static_cast<std::size_t>(start);
  if (replace > data_.size() - s) {
    replace = data_.size() - s;
  }
  data_.replace(s, replace, std::string(data.data(), data.size()));
  pos_ = start + static_cast<TagLib::offset_t>(data.size());
  length_ = static_cast<TagLib::offset_t>(data_.size());
}

void WebdavStream::removeBlock(TagLib::offset_t start, std::size_t length) {
  if (read_only_) {
    return;
  }
  if (start < 0) {
    start = 0;
  }
  const std::size_t s = static_cast<std::size_t>(start);
  if (s > data_.size()) {
    return;
  }
  if (length > data_.size() - s) {
    length = data_.size() - s;
  }
  data_.erase(s, length);
  length_ = static_cast<TagLib::offset_t>(data_.size());
}

void WebdavStream::seek(TagLib::offset_t offset, Position p) {
  if (p == Beginning) {
    pos_ = offset;
  } else if (p == Current) {
    pos_ += offset;
  } else if (p == End) {
    pos_ = length() + offset;
  }
  if (pos_ < 0) {
    pos_ = 0;
  }
}

void WebdavStream::truncate(TagLib::offset_t length) {
  if (read_only_) {
    return;
  }
  if (length < 0) {
    length = 0;
  }
  data_.resize(static_cast<std::size_t>(length));
  length_ = length;
  if (pos_ > length_) {
    pos_ = length_;
  }
}

bool WebdavStream::flush() {
  if (read_only_) {
    return false;
  }
  webdav::Response r = client_->put(path_, data_);
  return r.status >= 200 && r.status < 300;
}
