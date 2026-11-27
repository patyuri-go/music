#ifndef MEDIA_LIBRARY_PLAYBACK_H
#define MEDIA_LIBRARY_PLAYBACK_H

#include "media_library/byte_stream.h"
#include "media_library/domain.h"
#include "media_library/error.h"
#include "media_library/library.h"
#include "media_library/media_router.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace media_library {

// ---------------------------------------------------------------------------
// 时间来源。会话过期判断必须能注入，否则测试只能 sleep。
// ---------------------------------------------------------------------------

class Clock {
public:
  virtual ~Clock() = default;
  virtual std::int64_t now_ms() const = 0;
};

std::shared_ptr<Clock> make_system_clock();

// ---------------------------------------------------------------------------
// 7.1 播放会话
//
// 会话里没有 locator 的任何形态：locator 只活在 PlaybackService 内部的会话表里。
// 门面拿到的 PlaybackSession 可以安全序列化进 HTTP 响应。
// ---------------------------------------------------------------------------

struct PlaybackSourceSummary {
  TrackRef track;
  std::string mime_type;
  int bitrate_kbps = 0;
  std::int64_t duration_ms = 0;
  bool seekable = true;
};

struct PlaybackSession {
  std::string session_id;
  Sourced<Track> track;
  Sourced<PlaybackSourceSummary> stream;
  std::int64_t started_at_ms = 0;
  std::int64_t expires_at_ms = 0;
};

// ---------------------------------------------------------------------------
// 7.3 字节流
//
// 契约在 byte_stream.h（ByteStream）；AudioStream 是它的别名，
// 因为音频只是「可 seek 的字节流」。
// ---------------------------------------------------------------------------

// 无状态播放的一次结果：字节流 + 真正出流的库（用于 X-Musicd-Origin）。
struct OpenedStream {
  std::unique_ptr<AudioStream> stream;
  std::string library_id;
};

// 把 PlaybackSource 变成字节流：LocalFile/Cache 走文件，WebDav/RemoteStream
// 走 HTTP。音频与封面共用这一层，封面只需要 kind + locator 描述。
Result<std::unique_ptr<AudioStream>>
make_audio_stream(const PlaybackSource &source);

class PlaybackService {
public:
  virtual ~PlaybackService() = default;

  // 两次 resolve_*：元数据与流各自独立选源（7.2）。会话表存 locator。
  virtual Result<PlaybackSession> open_playback(const TrackRef &track,
                                                const CallContext &ctx) = 0;
  virtual Result<std::unique_ptr<AudioStream>>
  open_stream(const std::string &session_id, const CallContext &ctx) = 0;

  // 为一个 HTTP 请求临时选源并打开字节流；不建会话。
  // 无状态播放（/tracks/<ref>/stream 等）用它，免得为了拿 locator
  // 多解析一次元数据。同一 ref 的每个请求都会重新选源。
  virtual Result<OpenedStream> open_stream_for(const TrackRef &track,
                                               const CallContext &ctx) = 0;

  // 只读查询：不刷新、不改 locator。
  virtual Result<PlaybackSession> session(const std::string &session_id) = 0;

  // 到期前重新 resolve_stream，换新 session_id，旧 id 立即失效（7.4）。
  virtual Result<PlaybackSession> refresh(const std::string &session_id,
                                          const CallContext &ctx) = 0;

  virtual Status close(const std::string &session_id) = 0;
};

struct PlaybackServiceOptions {
  // 单会话上限。真实来源还会用自己的 ttl 进一步收紧（取两者较小值）。
  std::int64_t session_ttl_ms = 30 * 60 * 1000;
  // 会话表容量，超出按最久未访问淘汰；淘汰不是错误。
  std::size_t max_sessions = 32;
};

std::shared_ptr<PlaybackService>
make_playback_service(std::shared_ptr<MediaRouter> router,
                      std::shared_ptr<Clock> clock,
                      const PlaybackServiceOptions &options = {});

} // namespace media_library

#endif // MEDIA_LIBRARY_PLAYBACK_H
