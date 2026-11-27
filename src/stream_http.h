#ifndef STREAM_HTTP_H
#define STREAM_HTTP_H

#include "http_server.h"

#include "media_library/byte_stream.h"
#include "media_library/domain.h"
#include "media_library/error.h"
#include "media_library/playback.h"

#include <functional>
#include <memory>
#include <string>

// HTTP 侧的字节出口：把 ByteStream 按 Range 语义写成响应。
//
// 这是全进程唯一允许「把 locator 变成字节」的地方；locator 只经 PlaybackService
// 或 ArtworkService 流进来，不会出现在响应头或响应体里。
//
// 所有出字节的路由都走这里的 async 形态：由音频池/图片池的 worker 打开上游、
// 探总长、建响应，再用 Drogon 的异步流逐块推送。IO 线程只做参数解析与入队。
namespace stream_http {

using http_server::RequestPtr;
using http_server::ResponsePtr;

// callback 可能在工作线程上被调用（Drogon 保证可从任意线程回调）。
using AsyncCallback = std::function<void(const ResponsePtr &)>;

// ErrorCode -> HTTP 状态码。全进程只此一份。
int status_of(media_library::ErrorCode code) noexcept;

// 门面协议：**只决定错误响应体的形态**，字节出口与响应头完全一致。
//
// - `Native`：`/api/` 的统一 JSON（`{"error":{...}}`）。
// - `Subsonic`：`{"subsonic-response":{status:"failed",...}}`，且按规范用
//   HTTP 200 返回（错误只体现在信封里）。
// - `Jellyfin`：`ProblemDetails` JSON（`{type,title,status,detail}`）+ 真实
//   状态码。
enum class Protocol { Native, Subsonic, Jellyfin };

// 运行时参数（来自 config 的 http_stream 段）。启动时注入一次。
//
// 上游水位不在这里：它属于字节层（`media_library::set_upstream_buffer_bytes`），
// HTTP 层只需要超时。
struct StreamHttpOptions {
  int send_timeout_ms = 30000;
};
void configure(const StreamHttpOptions &options);

// 失败的原生响应体：{"error":{code,message,library_id,capability,restriction,
// retryable,retry_after}}；details / internal_cause 永不输出。
ResponsePtr error_response(const media_library::Error &error);

// 按门面协议渲染错误体（状态码见上方 Protocol 注释）。`content-length` 等
// 响应头不分协议；`Retry-After` 在三种协议下都照发。
ResponsePtr error_response_for(Protocol protocol,
                               const media_library::Error &error);

// 播放会话出流（/api/v1/playback/<id>/stream）。只有原生门面用到。
void serve_session_async(const RequestPtr &req, const std::string &session_id,
                         bool head_only, AsyncCallback callback);

// 一步式无状态出流：open_stream_for -> prepare -> push。给只认「一个 URL
// 出音频」的旧客户端（/api/v1/tracks/<ref>/stream、Jellyfin /Audio/...、
// Subsonic stream/download）用。
void serve_track_async(const RequestPtr &req,
                       const media_library::TrackRef &track, bool head_only,
                       Protocol protocol, AsyncCallback callback);

// 封面字节：ArtworkService::open -> prepare，附 ETag / Cache-Control，
// 命中 If-None-Match 时回 304。本地库与远端库同一入口。
void serve_artwork_async(const RequestPtr &req,
                         const media_library::ArtworkRef &artwork,
                         bool head_only, Protocol protocol,
                         AsyncCallback callback);

} // namespace stream_http

#endif // STREAM_HTTP_H
