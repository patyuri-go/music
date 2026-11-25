#include "api/subsonic/envelope.h"

#include "http_util.h"
#include "json_util.h"

namespace subsonic {

Code code_of(media_library::ErrorCode code) noexcept {
  using media_library::ErrorCode;
  switch (code) {
  case ErrorCode::NotFound:
    return Code::NotFound;
  case ErrorCode::InvalidRequest:
    return Code::AuthFailed;
  case ErrorCode::AuthRequired:
  case ErrorCode::PermissionDenied:
    return Code::AuthorizationFailed;
  case ErrorCode::Unsupported:
    return Code::ServerTooOld;
  default:
    // RateLimited / Timeout / ProviderUnavailable / Internal / ...
    // Subsonic 没有对应码，走通用失败（0）并在 message 里说明。
    return Code::Ok;
  }
}

std::string error_name(media_library::ErrorCode code) {
  return std::string(media_library::error_code_name(code));
}

http_server::ResponsePtr json_response(const Json::Value &body) {
  return http_util::jsonResponse(200, json_util::dump(body));
}

http_server::ResponsePtr failure_response(Code code, const std::string &message) {
  Json::Value error;
  error["code"] = static_cast<int>(code);
  error["message"] = message;

  Json::Value body;
  body["status"] = "failed";
  body["version"] = kApiVersion;
  body["type"] = "musicd";
  body["error"] = std::move(error);

  Json::Value root;
  root["subsonic-response"] = std::move(body);
  // Subsonic 规范里错误也用 HTTP 200 返回，错误只在信包里。
  return json_response(root);
}

http_server::ResponsePtr ok_response(Json::Value payload,
                           const std::string &server_name,
                           const std::string &server_version) {
  Json::Value body;
  body["status"] = "ok";
  body["version"] = kApiVersion;
  body["type"] = server_name.empty() ? "musicd" : server_name;
  body["serverVersion"] = server_version;
  body["openSubsonic"] = true;

  // payload 展开到同一层（Subsonic 的负载就在 subsonic-response 里）。
  if (payload.isObject()) {
    for (const std::string &key : payload.getMemberNames()) {
      body[key] = std::move(payload[key]);
    }
  }

  Json::Value root;
  root["subsonic-response"] = std::move(body);
  return json_response(root);
}

} // namespace subsonic
