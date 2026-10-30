#include "media_library/playback.h"

#include "api/webdav/webdav.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace media_library {
namespace {

constexpr std::size_t kReadChunk = 65536;

class SystemClock final : public Clock {
public:
  std::int64_t now_ms() const override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
};

std::string make_session_id() {
  static thread_local std::mt19937_64 engine{std::random_device{}()};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (int i = 0; i < 4; ++i) {
    std::uint64_t value = engine();
    for (int j = 0; j < 16; ++j) {
      out.push_back(kHex[(value >> 60) & 0x0F]);
      value <<= 4;
    }
  }
  return out;
}

Error internal_error(std::string message) {
  return Error::make(ErrorCode::Internal, std::move(message));
}

// ---------------------------------------------------------------------------
// 本地文件流
// ---------------------------------------------------------------------------

class LocalFileAudioStream final : public AudioStream {
public:
  static Result<std::unique_ptr<AudioStream>> open(const std::string &path,
                                                   std::string mime_type) {
    auto file = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!*file) {
      return Result<std::unique_ptr<AudioStream>>::failure(
          Error::make(ErrorCode::NotFound, "audio file not found"));
    }
    file->seekg(0, std::ios::end);
    const std::streamoff size = file->tellg();
    file->seekg(0, std::ios::beg);
    if (!*file) {
      return Result<std::unique_ptr<AudioStream>>::failure(
          internal_error("cannot stat audio file"));
    }
    StreamInfo info;
    info.mime_type = std::move(mime_type);
    info.size_bytes = static_cast<std::int64_t>(size);
    info.seekable = true;
    std::unique_ptr<AudioStream> stream(
        new LocalFileAudioStream(std::move(file), std::move(info)));
    return Result<std::unique_ptr<AudioStream>>::success(std::move(stream));
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
      return Status::failure(internal_error("cannot seek audio file"));
    }
    offset_ = offset;
    return Status::success();
  }

private:
  LocalFileAudioStream(std::unique_ptr<std::ifstream> file, StreamInfo info)
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

class RemoteAudioStream final : public AudioStream {
public:
  static Result<std::unique_ptr<AudioStream>> open(const PlaybackSource &source) {
    if (source.internal_locator.empty()) {
      return Result<std::unique_ptr<AudioStream>>::failure(
          internal_error("playback source has no locator"));
    }
    StreamInfo info;
    info.mime_type = source.mime_type;
    info.size_bytes = source.content_length.value_or(-1);
    info.seekable = source.range_supported;
    std::unique_ptr<AudioStream> stream(
        new RemoteAudioStream(source.internal_locator, std::move(info)));
    return Result<std::unique_ptr<AudioStream>>::success(std::move(stream));
  }

  ~RemoteAudioStream() override { close_handle(); }

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
          Error::make(ErrorCode::Unsupported, "audio stream is not seekable"));
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
  RemoteAudioStream(std::string locator, StreamInfo info)
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
                               "cannot open remote audio stream");
      open_error_.retryable = stream.status == 0;
      return false;
    }
    if (info_.size_bytes < 0) {
      if (stream.length >= 0) {
        info_.size_bytes =
            stream.length + (stream.status == 206 ? offset_ : 0);
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

Result<std::unique_ptr<AudioStream>> make_audio_stream(
    const PlaybackSource &source) {
  if (source.internal_locator.empty()) {
    return Result<std::unique_ptr<AudioStream>>::failure(
        internal_error("playback source has no locator"));
  }
  switch (source.kind) {
  case PlaybackKind::LocalFile:
  case PlaybackKind::Cache:
    return LocalFileAudioStream::open(source.internal_locator, source.mime_type);
  case PlaybackKind::WebDav:
  case PlaybackKind::RemoteStream:
    return RemoteAudioStream::open(source);
  }
  return Result<std::unique_ptr<AudioStream>>::failure(
      Error::make(ErrorCode::Unsupported, "unsupported playback kind"));
}

// ---------------------------------------------------------------------------
// 会话实现
// ---------------------------------------------------------------------------

class PlaybackServiceImpl final : public PlaybackService {
public:
  PlaybackServiceImpl(std::shared_ptr<MediaRouter> router,
                      std::shared_ptr<Clock> clock,
                      PlaybackServiceOptions options)
      : router_(std::move(router)), clock_(std::move(clock)),
        options_(options) {
    if (router_ == nullptr) {
      throw std::invalid_argument("playback service: router is null");
    }
    if (clock_ == nullptr) {
      throw std::invalid_argument("playback service: clock is null");
    }
  }

  Result<PlaybackSession> open_playback(const TrackRef &track,
                                        const CallContext &ctx) override {
    if (track.entity_type != EntityType::Track) {
      return Result<PlaybackSession>::failure(
          Error::make(ErrorCode::InvalidRequest, "expected a track reference"));
    }

    // 两次解析各自独立选源：允许元数据与流落在不同库（7.2）。
    Result<Sourced<Track>> metadata = router_->resolve_track(track, ctx);
    Result<Sourced<PlaybackSource>> stream = router_->resolve_stream(track, ctx);

    // 两步都失败时优先返回 stream 的错误：播放请求的口径是「能不能播」。
    if (!stream.ok()) {
      return Result<PlaybackSession>::failure(stream.error());
    }
    if (!metadata.ok()) {
      return Result<PlaybackSession>::failure(metadata.error());
    }
    const PlaybackSource &source = stream.value().value;
    if (source.internal_locator.empty()) {
      return Result<PlaybackSession>::failure(
          internal_error("playback source has no locator"));
    }

    const std::int64_t now = clock_->now_ms();
    std::int64_t ttl = options_.session_ttl_ms;
    if (source.expires_at.has_value()) {
      if (*source.expires_at <= now) {
        Error error = Error::make(ErrorCode::PlaybackExpired,
                                  "playback source already expired");
        error.retryable = true;
        return Result<PlaybackSession>::failure(std::move(error));
      }
      ttl = std::min(ttl, *source.expires_at - now);
    }

    PlaybackSession session;
    session.session_id = make_session_id();
    session.track = std::move(metadata.value());
    session.started_at_ms = now;
    session.expires_at_ms = now + ttl;

    PlaybackSourceSummary summary;
    summary.track = track;
    summary.mime_type = source.mime_type.empty() ? session.track.value.audio.format
                                                 : source.mime_type;
    summary.bitrate_kbps = source.bitrate_kbps.value_or(
        session.track.value.audio.bitrate_kbps.value_or(0));
    summary.duration_ms = session.track.value.duration_ms.value_or(0);
    summary.seekable = source.range_supported;
    session.stream.value = std::move(summary);
    session.stream.origin = std::move(stream.value().origin);

    const PlaybackSession copy = session;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Entry entry;
      entry.source = std::move(stream.value().value);
      entry.session = session;
      entry.last_access_ms = now;
      sessions_[entry.session.session_id] = std::move(entry);
      evict_locked();
    }
    return Result<PlaybackSession>::success(copy);
  }

  Result<std::unique_ptr<AudioStream>>
  open_stream(const std::string &session_id, const CallContext &) override {
    PlaybackSource source;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = sessions_.find(session_id);
      if (it == sessions_.end()) {
        return Result<std::unique_ptr<AudioStream>>::failure(
            Error::make(ErrorCode::NotFound, "playback session not found"));
      }
      const std::int64_t now = clock_->now_ms();
      if (now >= it->second.session.expires_at_ms) {
        sessions_.erase(it);
        Error error =
            Error::make(ErrorCode::PlaybackExpired, "playback session expired");
        error.retryable = true;
        return Result<std::unique_ptr<AudioStream>>::failure(std::move(error));
      }
      it->second.last_access_ms = now;
      source = it->second.source;
    }
    return make_audio_stream(source);
  }

  Result<PlaybackSession> session(const std::string &session_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
      return Result<PlaybackSession>::failure(
          Error::make(ErrorCode::NotFound, "playback session not found"));
    }
    if (clock_->now_ms() >= it->second.session.expires_at_ms) {
      sessions_.erase(it);
      Error error =
          Error::make(ErrorCode::PlaybackExpired, "playback session expired");
      error.retryable = true;
      return Result<PlaybackSession>::failure(std::move(error));
    }
    return Result<PlaybackSession>::success(it->second.session);
  }

  Result<PlaybackSession> refresh(const std::string &session_id,
                                  const CallContext &ctx) override {
    TrackRef track;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = sessions_.find(session_id);
      if (it == sessions_.end()) {
        return Result<PlaybackSession>::failure(
            Error::make(ErrorCode::NotFound, "playback session not found"));
      }
      track = it->second.session.track.value.ref;
      // 刷新先作废旧 id：失败就进 expired，不静默延长（7.4）。
      sessions_.erase(it);
    }
    return open_playback(track, ctx);
  }

  Status close(const std::string &session_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(session_id);
    return Status::success();
  }

private:
  struct Entry {
    PlaybackSource source;
    PlaybackSession session;
    std::int64_t last_access_ms = 0;
  };

  void evict_locked() {
    while (sessions_.size() > options_.max_sessions) {
      auto victim = sessions_.end();
      for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if (victim == sessions_.end() ||
            it->second.last_access_ms < victim->second.last_access_ms) {
          victim = it;
        }
      }
      if (victim == sessions_.end()) {
        return;
      }
      sessions_.erase(victim);
    }
  }

  std::shared_ptr<MediaRouter> router_;
  std::shared_ptr<Clock> clock_;
  PlaybackServiceOptions options_;
  std::mutex mutex_;
  std::unordered_map<std::string, Entry> sessions_;
};

} // namespace

std::shared_ptr<Clock> make_system_clock() {
  return std::make_shared<SystemClock>();
}

std::shared_ptr<PlaybackService>
make_playback_service(std::shared_ptr<MediaRouter> router,
                      std::shared_ptr<Clock> clock,
                      const PlaybackServiceOptions &options) {
  return std::make_shared<PlaybackServiceImpl>(std::move(router),
                                               std::move(clock), options);
}

} // namespace media_library
