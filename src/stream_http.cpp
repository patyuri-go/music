#include "stream_http.h"

#include "crow/json.h"
#include "crow/stream_response.h"
#include "http_util.h"
#include "media_library/media_runtime.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
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

crow::json::wvalue error_json(const Error &error) {
  crow::json::wvalue detail;
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
    detail["retry_after"] = *error.retry_after;
  }
  // details 与 internal_cause 是诊断字段，绝不出现在响应里。
  crow::json::wvalue body;
  body["error"] = std::move(detail);
  return body;
}

std::shared_ptr<PlaybackService> playback() {
  return media_library::playback_service_ptr();
}

} // namespace

int status_of(ErrorCode code) noexcept { return status_for(code); }

crow::response error_response(const Error &error) {
  crow::response res = http_util::jsonResponse(
      status_for(error.code), error_json(error).dump());
  if (error.retry_after.has_value()) {
    res.add_header("Retry-After", std::to_string(*error.retry_after));
  }
  return res;
}

crow::response serve(const crow::request &req,
                     std::shared_ptr<media_library::ByteStream> stream,
                     const std::string &library_id, bool head_only) {
  if (stream == nullptr) {
    return error_response(
        Error::make(ErrorCode::Internal, "audio stream is null"));
  }

  const StreamInfo info = stream->info();
  const std::string range = req.get_header_value("Range");

  std::int64_t size = info.size_bytes;
  std::int64_t start = 0;
  std::int64_t end = size >= 0 ? size - 1 : -1;

  if (size >= 0 && !range.empty()) {
    const auto [s, e] = http_util::parseRange(range, size);
    if (s < 0) {
      crow::response res(416, "range not satisfiable");
      res.add_header("Content-Range", "bytes */" + std::to_string(size));
      return res;
    }
    start = s;
    end = e;
  }

  // 长度未知就不承诺 Range：忽略 Range 头，顺序输出整段。
  if (start > 0 && !info.seekable) {
    crow::response res(416, "range not satisfiable");
    if (size >= 0) {
      res.add_header("Content-Range", "bytes */" + std::to_string(size));
    }
    return res;
  }
  if (start > 0) {
    media_library::Status seeked = stream->seek(start);
    if (!seeked.ok()) {
      return error_response(seeked.error());
    }
  }

  const std::int64_t length = (size >= 0 && end >= 0) ? end - start + 1 : -1;
  const int code = (start != 0 || (size >= 0 && end != size - 1)) ? 206 : 200;

  if (head_only) {
    crow::response res(code);
    if (!info.mime_type.empty()) {
      res.set_header("Content-Type", info.mime_type);
    }
    if (length >= 0) {
      res.set_header("Content-Length", std::to_string(length));
      res.manual_length_header = true;
    }
    res.set_header("Accept-Ranges", info.seekable ? "bytes" : "none");
    if (size >= 0) {
      res.set_header("Content-Range",
                     "bytes " + std::to_string(start) + "-" +
                         std::to_string(end) + "/" + std::to_string(size));
    }
    if (!library_id.empty()) {
      res.set_header("X-Musicd-Origin", library_id);
    }
    return res;
  }

  auto remaining = std::make_shared<std::int64_t>(length);
  crow::response res = crow::streamedResponse(
      code, info.mime_type, length,
      [stream, remaining](std::string &out) -> bool {
        if (*remaining == 0) {
          return false;
        }
        std::size_t want = 65536;
        if (*remaining > 0) {
          want = static_cast<std::size_t>(
              std::min<std::int64_t>(*remaining, 65536));
        }
        media_library::Result<std::vector<std::byte>> chunk =
            stream->read(want);
        if (!chunk.ok() || chunk.value().empty()) {
          return false;
        }
        const std::size_t got = chunk.value().size();
        out.resize(got);
        std::memcpy(out.data(), chunk.value().data(), got);
        if (*remaining > 0) {
          *remaining -= static_cast<std::int64_t>(got);
        }
        return true;
      });
  res.add_header("Accept-Ranges", info.seekable ? "bytes" : "none");
  if (size >= 0) {
    res.add_header("Content-Range", "bytes " + std::to_string(start) + "-" +
                                        std::to_string(end) + "/" +
                                        std::to_string(size));
  }
  if (!library_id.empty()) {
    res.add_header("X-Musicd-Origin", library_id);
  }
  return res;
}

crow::response serve_session(const crow::request &req,
                             const std::string &session_id, bool head_only) {
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

crow::response serve_track(const crow::request &req,
                           const media_library::TrackRef &track,
                           bool head_only) {
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
  crow::response res = serve(req, std::move(stream),
                             session.value().stream.origin.library_id,
                             head_only);
  res.add_header("X-Playback-Session", session.value().session_id);
  return res;
}

crow::response serve_artwork(const crow::request &req,
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
  if (!etag.empty()) {
    const auto &request_etag = req.get_header_value("If-None-Match");
    if (!request_etag.empty() && request_etag == etag) {
      crow::response not_modified(304);
      not_modified.add_header("ETag", etag);
      not_modified.add_header("Cache-Control", "public, max-age=3600");
      return not_modified;
    }
  }

  Result<std::unique_ptr<media_library::ByteStream>> opened =
      service->open(artwork, ctx);
  if (!opened.ok()) {
    return error_response(opened.error());
  }

  std::shared_ptr<media_library::ByteStream> stream(std::move(opened).value());
  crow::response res =
      serve(req, std::move(stream), info.value().library_id, head_only);
  if (!etag.empty()) {
    res.add_header("ETag", etag);
  }
  res.add_header("Cache-Control", "public, max-age=3600");
  return res;
}

} // namespace stream_http
