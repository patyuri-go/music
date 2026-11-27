#include "media_library/byte_stream.h"

#include "http_client.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <fstream>
#include <functional>
#include <mutex>
#include <utility>

namespace media_library {
namespace {

constexpr std::size_t kReadChunk = 65536;

Error internal_error(std::string message) {
  return Error::make(ErrorCode::Internal, std::move(message));
}

// 启动时注入的上游水位；0 表示交给 http_client 用默认值。
std::atomic<std::size_t> g_upstream_buffer_bytes{0};

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
        new FileByteStream(std::move(file), std::move(info), path));
    return Result<std::unique_ptr<ByteStream>>::success(std::move(stream));
  }

  StreamInfo info() const override { return info_; }

  std::optional<std::string> local_path() const override { return path_; }

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
  FileByteStream(std::unique_ptr<std::ifstream> file, StreamInfo info,
                 std::string path)
      : file_(std::move(file)), info_(std::move(info)), path_(std::move(path)) {}

  std::unique_ptr<std::ifstream> file_;
  StreamInfo info_;
  std::string path_;
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
       std::optional<std::int64_t> size, bool seekable,
       std::int64_t open_timeout_ms, int max_redirects,
       std::size_t buffer_bytes) {
    if (locator.empty()) {
      return Result<std::unique_ptr<ByteStream>>::failure(
          internal_error("byte stream has no locator"));
    }
    StreamInfo info;
    info.mime_type = std::move(mime_type);
    info.size_bytes = size.value_or(-1);
    info.seekable = seekable;
    std::unique_ptr<ByteStream> stream(new HttpByteStream(
        std::move(locator), std::move(info), open_timeout_ms, max_redirects,
        buffer_bytes));
    return Result<std::unique_ptr<ByteStream>>::success(std::move(stream));
  }

  ~HttpByteStream() override { close_handle(); }

  StreamInfo info() const override { return info_; }

  // 打开连接并从响应头补齐长度/mime；失败时把错误留给调用方。
  Status prepare() override {
    if (!ensure_open()) {
      return Status::failure(open_error_);
    }
    return Status::success();
  }

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
    if (got == 0) {
      // 正常 EOF：上游读完了。
      close_handle();
      return Result<std::vector<std::byte>>::success({});
    }
    if (got < 0) {
      // 传输错误或已取消。绝不能当成 EOF —— 否则客户端会把截断的音频
      // 当作完整文件静默接受。
      const bool was_cancelled = cancelled_.load();
      close_handle();
      if (was_cancelled) {
        return Result<std::vector<std::byte>>::failure(
            Error::make(ErrorCode::Cancelled, "remote stream cancelled"));
      }
      return Result<std::vector<std::byte>>::failure(
          Error::make(ErrorCode::ProviderUnavailable,
                      "remote stream interrupted"));
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

  void cancel() override {
    // 幂等 + 可从任意线程调用。这里只碰「取消闭包」和标志，不碰 open_ /
    // handle_：它们归读线程所有，跨线程修改会造成数据竞争。
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lock(cancel_mutex_);
      cancelled_ = true;
      fn = cancel_fn_;
    }
    if (fn) {
      fn(); // SharedStream::cancel() 本身线程安全，会唤醒阻塞的 read
    }
  }

private:
  HttpByteStream(std::string locator, StreamInfo info,
                 std::int64_t open_timeout_ms, int max_redirects,
                 std::size_t buffer_bytes)
      : locator_(std::move(locator)), info_(std::move(info)),
        open_timeout_ms_(open_timeout_ms), max_redirects_(max_redirects),
        buffer_bytes_(buffer_bytes) {}

  // 显式传入优先；否则用启动时注入的全局值；两者都没有就交给
  // http_client 的默认水位。
  std::size_t effective_buffer_bytes() const {
    if (buffer_bytes_ > 0) {
      return buffer_bytes_;
    }
    return g_upstream_buffer_bytes.load();
  }

  bool ensure_open() {
    if (open_) {
      return true;
    }    if (cancelled_.load()) {
      open_error_ = Error::make(ErrorCode::Cancelled, "remote stream cancelled");
      return false;
    }
    http_client::Url base;
    if (!http_client::parseUrl(locator_, base)) {
      open_error_ = Error::make(ErrorCode::ProviderUnavailable,
                               "cannot parse remote locator");
      return false;
    }
    std::vector<std::string> extra;
    if (info_.seekable) {
      extra.push_back("Range: bytes=" + std::to_string(offset_) + "-");
    }
    http_client::Stream stream = http_client::openStream(
        base, base.path, extra, open_timeout_ms_, max_redirects_,
        effective_buffer_bytes());
    if (stream.status != 200 && stream.status != 206) {
      if (stream.close) {
        stream.close();
      }
      if (stream.timed_out) {
        open_error_ = Error::make(ErrorCode::Timeout,
                                 "remote stream timed out");
      } else {
        open_error_ = Error::make(ErrorCode::ProviderUnavailable,
                                 "cannot open remote stream");
        open_error_.retryable = stream.status == 0;
      }
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
    {
      // 抓一份取消闭包，供 cancel() 从别的线程安全调用。
      std::lock_guard<std::mutex> lock(cancel_mutex_);
      cancel_fn_ = handle_.close;
    }
    open_ = true;
    return true;
  }

  void close_handle() {
    if (open_) {
      std::function<void()> fn;
      {
        std::lock_guard<std::mutex> lock(cancel_mutex_);
        fn = handle_.close;
        cancel_fn_ = nullptr;
      }
      if (fn) {
        fn();
      }
      open_ = false;
    }
  }

  std::string locator_;
  StreamInfo info_;
  std::int64_t offset_ = 0;
  std::int64_t open_timeout_ms_ = 0;
  // > 0 时跟随 3xx：存储节点可以把直链以 302 交出来。
  int max_redirects_ = 0;
  // 0 表示沿用启动时注入的全局水位。
  std::size_t buffer_bytes_ = 0;
  http_client::Stream handle_;
  bool open_ = false;
  Error open_error_;
  // cancel() 可跨线程调用，所以取消闭包与标志各自有锁；open_ / handle_
  // 仍只归读线程所有，避免竞争。
  mutable std::mutex cancel_mutex_;
  std::function<void()> cancel_fn_;
  std::atomic<bool> cancelled_{false};
};

} // namespace

Result<std::unique_ptr<ByteStream>> make_file_stream(std::string path,
                                                     std::string mime_type) {
  return FileByteStream::open(path, std::move(mime_type));
}

Result<std::unique_ptr<ByteStream>>
make_http_stream(std::string locator, std::string mime_type,
                 std::optional<std::int64_t> size, bool seekable,
                 std::int64_t open_timeout_ms, int max_redirects,
                 std::size_t buffer_bytes) {
  return HttpByteStream::open(std::move(locator), std::move(mime_type), size,
                              seekable, open_timeout_ms, max_redirects,
                              buffer_bytes);
}

void set_upstream_buffer_bytes(std::size_t bytes) {
  g_upstream_buffer_bytes.store(bytes);
}

} // namespace media_library
