#include "api/subsonic/envelope.h"

#include "http_util.h"

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

crow::response json_response(const crow::json::wvalue &body) {
  return http_util::jsonResponse(200, body.dump());
}

crow::response failure_response(Code code, const std::string &message) {
  crow::json::wvalue error;
  error["code"] = static_cast<int>(code);
  error["message"] = message;

  crow::json::wvalue body;
  body["status"] = "failed";
  body["version"] = kApiVersion;
  body["type"] = "musicd";
  body["error"] = std::move(error);

  crow::json::wvalue root;
  root["subsonic-response"] = std::move(body);
  // Subsonic 规范里错误也用 HTTP 200 返回，错误只在信包里。
  return json_response(root);
}

crow::response ok_response(crow::json::wvalue payload,
                           const std::string &server_name,
                           const std::string &server_version) {
  crow::json::wvalue body;
  body["status"] = "ok";
  body["version"] = kApiVersion;
  body["type"] = server_name.empty() ? "musicd" : server_name;
  body["serverVersion"] = server_version;
  body["openSubsonic"] = true;

  // payload 展开到同一层（Subsonic 的负载就在 subsonic-response 里）。
  if (payload.t() == crow::json::type::Object) {
    for (const std::string &key : payload.keys()) {
      body[key] = std::move(payload[key]);
    }
  }

  crow::json::wvalue root;
  root["subsonic-response"] = std::move(body);
  return json_response(root);
}

} // namespace subsonic
