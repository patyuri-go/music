#include "media_library/byte_stream.h"

#include "api/webdav/webdav.h"

#include <algorithm>
#include <fstream>
#include <utility>

namespace media_library {
namespace {

constexpr std::size_t kReadChunk = 65536;

Error internal_error(std::string message) {
  return Error::make(ErrorCode::Internal, std::move(message));
}

// ---------------------------------------------------------------------------
// 本地文件流
// ---------------------------------------------------------------------------

class FileByteStream final : public ByteStream {
public:
  static Result<std::unique_ptr<ByteStream>> open(const std::string &path,
                                                  std::string mime_type) {
    auto file = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!*file) {
      return Result<std::unique_ptr<ByteStream>>::failure(
          Error::make(ErrorCode::NotFound, "file not found"));
    }
    file->seekg(0, std::ios::end);
    const std::streamoff size = file->tellg();
    file->seekg(0, std::ios::beg);
    if (!*file) {
      return Result<std::unique_ptr<ByteStream>>::failure(
          internal_error("cannot stat file"));
    }
    StreamInfo info;
    info.mime_type = std::move(mime_type);
    info.size_bytes = static_cast<std::int64_t>(size);
    info.seekable = true;
    std::unique_ptr<ByteStream> stream(
        new FileByteStream(std::move(file), std::move(info)));
    return Result<std::unique_ptr<ByteStream>>::success(std::move(stream));
  }

  StreamInfo info() const override { return info_; }

  Result<std::vector<std::byte>> read(std::size_t max_bytes) override {
    if (max_bytes == 0) {
      return Result<std::vector<std::byte>>::success({});
    }
    std::size_t want = std::min<std::size_t>(max_bytes, kReadChunk);
    if (info_.size_bytes >= 0) {
      const std::int64_t remaining = info_.size_bytes - offset_;
      if (remaining <= 0) {
        return Result<std::vector<std::byte>>::success({});
      }
      if (static_cast<std::int64_t>(want) > remaining) {
        want = static_cast<std::size_t>(remaining);
      }
    }
    std::vector<std::byte> out(want);
    file_->read(reinterpret_cast<char *>(out.data()),
                static_cast<std::streamsize>(want));
    const std::streamsize got = file_->gcount();
    if (got <= 0) {
      file_->clear();
      return Result<std::vector<std::byte>>::success({});
    }
    out.resize(static_cast<std::size_t>(got));
    offset_ += got;
    return Result<std::vector<std::byte>>::success(std::move(out));
  }

  Status seek(std::int64_t offset) override {
    if (offset < 0) {
      return Status::failure(
          Error::make(ErrorCode::InvalidRequest, "negative seek offset"));
    }
    file_->clear();
    file_->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!*file_) {
      return Status::failure(internal_error("cannot seek file"));
    }
    offset_ = offset;
    return Status::success();
  }

private:
  FileByteStream(std::unique_ptr<std::ifstream> file, StreamInfo info)
      : file_(std::move(file)), info_(std::move(info)) {}

  std::unique_ptr<std::ifstream> file_;
  StreamInfo info_;
  std::int64_t offset_ = 0;
};

// ---------------------------------------------------------------------------
// 远端（WebDAV / HTTP）流
//
// webdav::Stream 是顺序句柄，seek 只能靠带 Range 重新开一次连接，
// 因此这里维护「当前连接对应的起始偏移」，seek 先关后置。
// ---------------------------------------------------------------------------

class HttpByteStream final : public ByteStream {
public:
  static Result<std::unique_ptr<ByteStream>>
  open(std::string locator, std::string mime_type,
       std::optional<std::int64_t> size, bool seekable) {
    if (locator.empty()) {
      return Result<std::unique_ptr<ByteStream>>::failure(
          internal_error("byte stream has no locator"));
    }
    StreamInfo info;
    info.mime_type = std::move(mime_type);
    info.size_bytes = size.value_or(-1);
    info.seekable = seekable;
    std::unique_ptr<ByteStream> stream(
        new HttpByteStream(std::move(locator), std::move(info)));
    return Result<std::unique_ptr<ByteStream>>::success(std::move(stream));
  }

  ~HttpByteStream() override { close_handle(); }

  StreamInfo info() const override { return info_; }

  Result<std::vector<std::byte>> read(std::size_t max_bytes) override {
    if (max_bytes == 0) {
      return Result<std::vector<std::byte>>::success({});
    }
    if (!ensure_open()) {
      return Result<std::vector<std::byte>>::failure(open_error_);
    }
    std::size_t want = std::min<std::size_t>(max_bytes, kReadChunk);
    if (info_.size_bytes >= 0) {
      const std::int64_t remaining = info_.size_bytes - offset_;
      if (remaining <= 0) {
        close_handle();
        return Result<std::vector<std::byte>>::success({});
      }
      if (static_cast<std::int64_t>(want) > remaining) {
        want = static_cast<std::size_t>(remaining);
      }
    }
    std::vector<std::byte> out(want);
    const long long got = handle_.read(reinterpret_cast<char *>(out.data()), want);
    if (got <= 0) {
      close_handle();
      return Result<std::vector<std::byte>>::success({});
    }
    out.resize(static_cast<std::size_t>(got));
    offset_ += got;
    return Result<std::vector<std::byte>>::success(std::move(out));
  }

  Status seek(std::int64_t offset) override {
    if (offset < 0) {
      return Status::failure(
          Error::make(ErrorCode::InvalidRequest, "negative seek offset"));
    }
    if (!info_.seekable) {
      return Status::failure(
          Error::make(ErrorCode::Unsupported, "stream is not seekable"));
    }
    if (info_.size_bytes >= 0 && offset > info_.size_bytes) {
      return Status::failure(
          Error::make(ErrorCode::InvalidRequest, "seek beyond end of stream"));
    }
    close_handle();
    offset_ = offset;
    return Status::success();
  }

private:
  HttpByteStream(std::string locator, StreamInfo info)
      : locator_(std::move(locator)), info_(std::move(info)) {}

  bool ensure_open() {
    if (open_) {
      return true;
    }
    const webdav::Url base = webdav::resolveCredentials(locator_);
    std::vector<std::string> extra;
    if (info_.seekable) {
      extra.push_back("Range: bytes=" + std::to_string(offset_) + "-");
    }
    webdav::Stream stream = webdav::openStream(base, base.path, extra);
    if (stream.status != 200 && stream.status != 206) {
      if (stream.close) {
        stream.close();
      }
      open_error_ = Error::make(ErrorCode::ProviderUnavailable,
                               "cannot open remote stream");
      open_error_.retryable = stream.status == 0;
      return false;
    }
    if (info_.size_bytes < 0) {
      if (stream.length >= 0) {
        info_.size_bytes = stream.length + (stream.status == 206 ? offset_ : 0);
      } else if (const auto it = stream.headers.find("content-range");
                 it != stream.headers.end()) {
        const std::size_t slash = it->second.find('/');
        if (slash != std::string::npos) {
          try {
            info_.size_bytes = std::stoll(it->second.substr(slash + 1));
          } catch (const std::exception &) {
            info_.size_bytes = -1;
          }
        }
      }
    }
    if (info_.mime_type.empty()) {
      const auto it = stream.headers.find("content-type");
      if (it != stream.headers.end()) {
        info_.mime_type = it->second;
      }
    }
    handle_ = std::move(stream);
    open_ = true;
    return true;
  }

  void close_handle() {
    if (open_) {
      if (handle_.close) {
        handle_.close();
      }
      open_ = false;
    }
  }

  std::string locator_;
  StreamInfo info_;
  std::int64_t offset_ = 0;
  webdav::Stream handle_;
  bool open_ = false;
  Error open_error_;
};

} // namespace

Result<std::unique_ptr<ByteStream>> make_file_stream(std::string path,
                                                     std::string mime_type) {
  return FileByteStream::open(path, std::move(mime_type));
}

Result<std::unique_ptr<ByteStream>>
make_http_stream(std::string locator, std::string mime_type,
                 std::optional<std::int64_t> size, bool seekable) {
  return HttpByteStream::open(std::move(locator), std::move(mime_type), size,
                              seekable);
}

} // namespace media_library
