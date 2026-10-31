#ifndef STREAM_HTTP_H
#define STREAM_HTTP_H

#include "crow/http_request.h"
#include "crow/http_response.h"

#include "media_library/byte_stream.h"
#include "media_library/domain.h"
#include "media_library/error.h"
#include "media_library/playback.h"

#include <memory>
#include <string>

// HTTP 侧的字节出口：把 ByteStream 按 Range 语义写成响应。
//
// 这是全进程唯一允许「把 locator 变成字节」的地方；locator 只经 PlaybackService
// 或 ArtworkService 流进来，不会出现在响应头或响应体里。
namespace stream_http {

// ErrorCode -> HTTP 状态码。全进程只此一份。
int status_of(media_library::ErrorCode code) noexcept;

// 失败的统一响应体：{"error":{code,message,library_id,capability,restriction,
// retryable,retry_after}}；details / internal_cause 永不输出。
crow::response error_response(const media_library::Error &error);

// 把一次已打开的流按请求的 Range 输出。
// `library_id` 只用于 X-Musicd-Origin 归因头（换源播放要可追溯）。
// 流的所有权转交给响应：读完后随响应对一起释放。
crow::response serve(const crow::request &req,
                     std::shared_ptr<media_library::ByteStream> stream,
                     const std::string &library_id, bool head_only);

// 打开 session 后输出；失败按 ErrorCode 映射成 JSON 错误。
crow::response serve_session(const crow::request &req,
                             const std::string &session_id, bool head_only);

// 一步式：open_playback -> open_stream -> serve。给只认「一个 URL 出音频」的
// 旧客户端（5666 /api/track/<id>/file、Jellyfin /Audio/<id>/stream）用。
crow::response serve_track(const crow::request &req,
                           const media_library::TrackRef &track, bool head_only);

// 封面字节：ArtworkService::info -> open -> serve，附 ETag / Cache-Control，
// 命中 If-None-Match 时回 304。本地库与远端库同一入口。
crow::response serve_artwork(const crow::request &req,
                             const media_library::ArtworkRef &artwork,
                             bool head_only);

} // namespace stream_http

#endif // STREAM_HTTP_H
