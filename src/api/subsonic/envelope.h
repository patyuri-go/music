#ifndef SUBSONIC_ENVELOPE_H
#define SUBSONIC_ENVELOPE_H

#include "crow/http_response.h"
#include "crow/json.h"
#include "media_library/error.h"

#include <string>

namespace subsonic {

// Subsonic 协议版本。Navidrome 实现的是 1.16.1，这里对齐。
constexpr const char *kApiVersion = "1.16.1";

// Subsonic 错误码（规范定义，不能自创）。
enum class Code : int {
  Ok = 0,
  MissingParameter = 10,
  ClientTooOld = 20,
  ServerTooOld = 30,
  AuthFailed = 40,
  AuthorizationFailed = 50,
  NotFound = 70,
};

// 内部 ErrorCode -> Subsonic code。能确定语义的精确对应；不能确定的
// 统一 Ok(0) 并在 message 里带上原始错误名。
Code code_of(media_library::ErrorCode code) noexcept;

// 内部 ErrorCode 的 snake_case 名（写日志用）。
std::string error_name(media_library::ErrorCode code);

// 构造一条错误响应：status=failed + error{code,message}。
crow::response failure_response(Code code, const std::string &message);

// 构造一条成功响应：把 payload 塞进 subsonic-response 信封。
// payload 为空对象时只输出 ping 那种最小信封。
crow::response ok_response(crow::json::wvalue payload,
                           const std::string &server_name,
                           const std::string &server_version);

// 统一的 JSON 出口（Content-Type: application/json; charset=utf-8）。
crow::response json_response(const crow::json::wvalue &body);

} // namespace subsonic

#endif // SUBSONIC_ENVELOPE_H
