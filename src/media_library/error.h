#ifndef MEDIA_LIBRARY_ERROR_H
#define MEDIA_LIBRARY_ERROR_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace media_library {

enum class ErrorCode {
  InvalidRequest,
  NotFound,
  AuthRequired,
  PermissionDenied,
  Unsupported,
  ProviderUnavailable,
  Timeout,
  RateLimited,
  PlaybackExpired,
  Conflict,
  Cancelled,
  Internal
};

enum class RestrictionCode {
  AuthRequired,
  PermissionDenied,
  RegionBlocked,
  VipRequired,
  CopyrightUnavailable,
  AccountRestricted,
  AgeRestricted,
  QualityUnavailable,
  Unknown
};

constexpr std::string_view error_code_name(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::InvalidRequest:
    return "invalid_request";
  case ErrorCode::NotFound:
    return "not_found";
  case ErrorCode::AuthRequired:
    return "auth_required";
  case ErrorCode::PermissionDenied:
    return "permission_denied";
  case ErrorCode::Unsupported:
    return "unsupported";
  case ErrorCode::ProviderUnavailable:
    return "provider_unavailable";
  case ErrorCode::Timeout:
    return "timeout";
  case ErrorCode::RateLimited:
    return "rate_limited";
  case ErrorCode::PlaybackExpired:
    return "playback_expired";
  case ErrorCode::Conflict:
    return "conflict";
  case ErrorCode::Cancelled:
    return "cancelled";
  case ErrorCode::Internal:
    return "internal";
  }
  return "internal";
}

constexpr std::string_view restriction_code_name(RestrictionCode code) noexcept {
  switch (code) {
  case RestrictionCode::AuthRequired:
    return "auth_required";
  case RestrictionCode::PermissionDenied:
    return "permission_denied";
  case RestrictionCode::RegionBlocked:
    return "region_blocked";
  case RestrictionCode::VipRequired:
    return "vip_required";
  case RestrictionCode::CopyrightUnavailable:
    return "copyright_unavailable";
  case RestrictionCode::AccountRestricted:
    return "account_restricted";
  case RestrictionCode::AgeRestricted:
    return "age_restricted";
  case RestrictionCode::QualityUnavailable:
    return "quality_unavailable";
  case RestrictionCode::Unknown:
    return "unknown";
  }
  return "unknown";
}

struct Error {
  ErrorCode code = ErrorCode::Internal;
  std::string message;
  std::string library_id;
  std::string capability;
  bool retryable = false;
  std::optional<std::int64_t> retry_after;
  // 限制类失败（版权、地区、VIP、音质）与故障类失败是两件事：前者换源也没用，
  // 必须让门面能翻译成用户可读原因。只在限制类失败里填写。
  std::optional<RestrictionCode> restriction;
  std::string details;
  std::string internal_cause;

  static Error make(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.message = std::move(message);
    return error;
  }
};

template <typename T, typename E = Error> class Result {
public:
  static Result success(T value) { return Result(std::move(value)); }
  static Result failure(E error) { return Result(std::move(error)); }

  bool ok() const noexcept { return std::holds_alternative<T>(data_); }

  T &value() & { return std::get<T>(data_); }
  const T &value() const & { return std::get<T>(data_); }
  T &&value() && { return std::get<T>(std::move(data_)); }

  E &error() & { return std::get<E>(data_); }
  const E &error() const & { return std::get<E>(data_); }

private:
  explicit Result(T value) : data_(std::move(value)) {}
  explicit Result(E error) : data_(std::move(error)) {}

  std::variant<T, E> data_;
};

template <typename E> class Result<void, E> {
public:
  static Result success() { return Result{}; }
  static Result failure(E error) { return Result(std::move(error)); }

  bool ok() const noexcept { return !error_.has_value(); }

  E &error() & { return *error_; }
  const E &error() const & { return *error_; }

private:
  Result() = default;
  explicit Result(E error) : error_(std::move(error)) {}

  std::optional<E> error_;
};

using Status = Result<void, Error>;

} // namespace media_library

#endif // MEDIA_LIBRARY_ERROR_H
