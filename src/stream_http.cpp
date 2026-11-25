#include "stream_http.h"

#include "http_util.h"
#include "json_util.h"
#include "media_library/media_runtime.h"
#include "storage_client.h"

#include <json/json.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace stream_http {
namespace {

using media_library::ByteStream;
using media_library::Error;
using media_library::ErrorCode;
using media_library::PlaybackService;
using media_library::PlaybackSession;
using media_library::Result;
using media_library::StreamInfo;

// 唯一一份 ErrorCode -> HTTP 状态码映射；门面不要再各自抄一遍。
int status_for(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::InvalidRequest:
    return 400;
  case ErrorCode::AuthRequired:
    return 401;
  case ErrorCode::PermissionDenied:
    return 403;
  case ErrorCode::NotFound:
    return 404;
  case ErrorCode::Conflict:
    return 409;
  case ErrorCode::PlaybackExpired:
    return 410;
  case ErrorCode::RateLimited:
    return 429;
  case ErrorCode::Unsupported:
    return 501;
  case ErrorCode::ProviderUnavailable:
    return 503;
  case ErrorCode::Timeout:
    return 504;
  case ErrorCode::Cancelled:
    return 499;
  case ErrorCode::Internal:
    return 500;
  }
  return 500;
}

Json::Value error_json(const Error &error) {
  Json::Value detail;
  detail["code"] = std::string(media_library::error_code_name(error.code));
  detail["message"] = error.message;
  if (!error.library_id.empty()) {
    detail["library_id"] = error.library_id;
  }
  if (!error.capability.empty()) {
    detail["capability"] = error.capability;
  }
  if (error.restriction.has_value()) {
    detail["restriction"] =
        std::string(media_library::restriction_code_name(*error.restriction));
  }
  detail["retryable"] = error.retryable;
  if (error.retry_after.has_value()) {
    detail["retry_after"] = static_cast<Json::Int64>(*error.retry_after);
  }
  // details 与 internal_cause 是诊断字段，绝不出现在响应里。
  Json::Value body;
  body["error"] = std::move(detail);
  return body;
}

std::shared_ptr<PlaybackService> playback() {
  return media_library::playback_service_ptr();
}

// 远端库仍是同步实现（打开上游连接后返回流式响应）。放到独立线程执行，
// 避免阻塞 Drogon 的 IO 线程；callback 由 Drogon 保证可以从任意线程调用。
void run_off_loop(std::function<ResponsePtr()> work, AsyncCallback callback) {
  std::thread([work = std::move(work),
               callback = std::move(callback)]() mutable {
    callback(work());
  }).detach();
}

ResponsePtr rangeNotSatisfiable(std::int64_t size) {
  ResponsePtr res = http_server::textResponse(416, "range not satisfiable");
  if (size >= 0) {
    res->addHeader("Content-Range", "bytes */" + std::to_string(size));
  }
  return res;
}

} // namespace

int status_of(ErrorCode code) noexcept { return status_for(code); }

ResponsePtr error_response(const Error &error) {
  ResponsePtr res = http_util::jsonResponse(status_for(error.code),
                                            json_util::dump(error_json(error)));
  if (error.retry_after.has_value()) {
    res->addHeader("Retry-After", std::to_string(*error.retry_after));
  }
  return res;
}

ResponsePtr serve(const RequestPtr &req,
                  std::shared_ptr<media_library::ByteStream> stream,
                  const std::string &library_id, bool head_only) {
  if (stream == nullptr) {
    return error_response(
        Error::make(ErrorCode::Internal, "audio stream is null"));
  }

  StreamInfo info = stream->info();
  // 远端流惰性连接：长度未知时先探测，否则 Content-Length 会退化成 0。
  if (info.size_bytes < 0) {
    const media_library::Status prepared = stream->prepare();
    if (!prepared.ok()) {
      return error_response(prepared.error());
    }
    info = stream->info();
  }
  const std::string range = req ? req->getHeader("Range") : std::string();

  std::int64_t size = info.size_bytes;
  std::int64_t start = 0;
  std::int64_t end = size >= 0 ? size - 1 : -1;

  if (size >= 0 && !range.empty()) {
    const auto [s, e] = http_util::parseRange(range, size);
    if (s < 0) {
      return rangeNotSatisfiable(size);
    }
    start = s;
    end = e;
  }

  // 长度未知就不承诺 Range：忽略 Range 头，顺序输出整段。
  if (start > 0 && !info.seekable) {
    return rangeNotSatisfiable(size);
  }
  if (start > 0) {
    media_library::Status seeked = stream->seek(start);
    if (!seeked.ok()) {
      return error_response(seeked.error());
    }
  }

  const std::int64_t length = (size >= 0 && end >= 0) ? end - start + 1 : -1;
  const int code = (start != 0 || (size >= 0 && end != size - 1)) ? 206 : 200;
  const std::string content_type =
      info.mime_type.empty() ? std::string("application/octet-stream")
                             : info.mime_type;

  if (head_only) {
    ResponsePtr res = drogon::HttpResponse::newHttpResponse();
    res->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
    res->setContentTypeString(content_type);
    if (length >= 0) {
      res->addHeader("Content-Length", std::to_string(length));
    }
    res->addHeader("Accept-Ranges", info.seekable ? "bytes" : "none");
    if (size >= 0) {
      res->addHeader("Content-Range", "bytes " + std::to_string(start) + "-" +
                                          std::to_string(end) + "/" +
                                          std::to_string(size));
    }
    if (!library_id.empty()) {
      res->addHeader("X-Musicd-Origin", library_id);
    }
    return res;
  }

  auto remaining = std::make_shared<std::int64_t>(length);
  ResponsePtr res = drogon::HttpResponse::newStreamResponse(
      [stream, remaining](char *buf, std::size_t len) -> std::size_t {
        // buf == nullptr 表示响应结束/连接中断，无需额外清理（shared_ptr 释放）。
        if (buf == nullptr || len == 0) {
          return 0;
        }
        if (*remaining == 0) {
          return 0;
        }
        std::size_t want = len;
        if (*remaining > 0) {
          want = static_cast<std::size_t>(
              std::min<std::int64_t>(*remaining, static_cast<std::int64_t>(len)));
        }
        media_library::Result<std::vector<std::byte>> chunk = stream->read(want);
        if (!chunk.ok() || chunk.value().empty()) {
          return 0;
        }
        const std::size_t got =
            std::min<std::size_t>(chunk.value().size(), len);
        std::memcpy(buf, chunk.value().data(), got);
        if (*remaining > 0) {
          *remaining -= static_cast<std::int64_t>(got);
        }
        return got;
      },
      "", drogon::CT_NONE, content_type);
  res->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
  if (length >= 0) {
    // 流式响应没有长度就是 chunked；这里显式声明长度。
    res->addHeader("Content-Length", std::to_string(length));
  }
  res->addHeader("Accept-Ranges", info.seekable ? "bytes" : "none");
  if (size >= 0) {
    res->addHeader("Content-Range", "bytes " + std::to_string(start) + "-" +
                                        std::to_string(end) + "/" +
                                        std::to_string(size));
  }
  if (!library_id.empty()) {
    res->addHeader("X-Musicd-Origin", library_id);
  }
  return res;
}

ResponsePtr serve_session(const RequestPtr &req, const std::string &session_id,
                          bool head_only) {
  const std::shared_ptr<PlaybackService> service = playback();
  if (service == nullptr) {
    return error_response(Error::make(ErrorCode::ProviderUnavailable,
                                      "media runtime is not initialized"));
  }
  Result<PlaybackSession> session = service->session(session_id);
  if (!session.ok()) {
    return error_response(session.error());
  }
  Result<std::unique_ptr<media_library::ByteStream>> opened =
      service->open_stream(session_id, media_library::CallContext{});
  if (!opened.ok()) {
    return error_response(opened.error());
  }
  std::shared_ptr<media_library::ByteStream> stream(std::move(opened).value());
  return serve(req, std::move(stream), session.value().stream.origin.library_id,
               head_only);
}

ResponsePtr serve_track(const RequestPtr &req,
                        const media_library::TrackRef &track, bool head_only) {
  const std::shared_ptr<PlaybackService> service = playback();
  if (service == nullptr) {
    return error_response(Error::make(ErrorCode::ProviderUnavailable,
                                      "media runtime is not initialized"));
  }
  Result<PlaybackSession> session =
      service->open_playback(track, media_library::CallContext{});
  if (!session.ok()) {
    return error_response(session.error());
  }
  Result<std::unique_ptr<media_library::ByteStream>> opened =
      service->open_stream(session.value().session_id,
                           media_library::CallContext{});
  if (!opened.ok()) {
    return error_response(opened.error());
  }
  std::shared_ptr<media_library::ByteStream> stream(std::move(opened).value());
  ResponsePtr res = serve(req, std::move(stream),
                          session.value().stream.origin.library_id, head_only);
  res->addHeader("X-Playback-Session", session.value().session_id);
  return res;
}

ResponsePtr serve_artwork(const RequestPtr &req,
                          const media_library::ArtworkRef &artwork,
                          bool head_only) {
  const std::shared_ptr<media_library::ArtworkService> service =
      media_library::artwork_service_ptr();
  if (service == nullptr) {
    return error_response(Error::make(ErrorCode::ProviderUnavailable,
                                      "media runtime is not initialized"));
  }

  const media_library::CallContext ctx;
  Result<media_library::ArtworkInfo> info = service->info(artwork, ctx);
  if (!info.ok()) {
    return error_response(info.error());
  }

  const std::string &etag = info.value().etag;
  if (!etag.empty() && req) {
    const std::string request_etag = req->getHeader("If-None-Match");
    if (!request_etag.empty() && request_etag == etag) {
      ResponsePtr not_modified = drogon::HttpResponse::newHttpResponse();
      not_modified->setStatusCode(drogon::k304NotModified);
      not_modified->addHeader("ETag", etag);
      not_modified->addHeader("Cache-Control", "public, max-age=3600");
      return not_modified;
    }
  }

  Result<std::unique_ptr<media_library::ByteStream>> opened =
      service->open(artwork, ctx);
  if (!opened.ok()) {
    return error_response(opened.error());
  }

  std::shared_ptr<media_library::ByteStream> stream(std::move(opened).value());
  ResponsePtr res =
      serve(req, std::move(stream), info.value().library_id, head_only);
  if (!etag.empty()) {
    res->addHeader("ETag", etag);
  }
  res->addHeader("Cache-Control", "public, max-age=3600");
  return res;
}

void serve_track_async(const RequestPtr &req,
                       const media_library::TrackRef &track, bool head_only,
                       AsyncCallback callback) {
  const auto internal_id = media_library::compat_internal_id_of(track);
  if (internal_id.has_value()) {
    // 本地库：唯一合法来源是存储服务；entity_id 缺失视为硬失败，
    // 不回退到任何本地路径（见 document/STORAGE_PROXY_DECISIONS.md）。
    const auto entity_id = media_library::compat_entity_id(
        media_library::EntityType::Track, *internal_id);
    if (!entity_id.has_value()) {
      callback(error_response(Error::make(
          ErrorCode::NotFound, "track has no entity id")));
      return;
    }
    if (!storage_client::enabled()) {
      callback(error_response(Error::make(
          ErrorCode::ProviderUnavailable, "storage service is not configured")));
      return;
    }
    storage_client::serve_content(req, *entity_id, false, head_only,
                                  std::move(callback));
    return;
  }
  run_off_loop([req, track, head_only] {
    return serve_track(req, track, head_only);
  }, std::move(callback));
}

void serve_artwork_async(const RequestPtr &req,
                         const media_library::ArtworkRef &artwork,
                         bool head_only, AsyncCallback callback) {
  const auto internal_id =
      media_library::compat_internal_id_of(artwork.owner);
  if (internal_id.has_value()) {
    const auto entity_id =
        media_library::compat_artwork_entity_id(artwork.owner);
    if (!entity_id.has_value()) {
      callback(error_response(Error::make(
          ErrorCode::NotFound, "album has no entity id")));
      return;
    }
    if (!storage_client::enabled()) {
      callback(error_response(Error::make(
          ErrorCode::ProviderUnavailable, "storage service is not configured")));
      return;
    }
    storage_client::serve_content(req, *entity_id, true, head_only,
                                  std::move(callback));
    return;
  }
  run_off_loop([req, artwork, head_only] {
    return serve_artwork(req, artwork, head_only);
  }, std::move(callback));
}

} // namespace stream_http