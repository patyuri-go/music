#include "api/api-enhanced/ids.h"

#include <limits>

namespace api_enhanced {
namespace {

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

} // namespace

media_library::Result<std::int64_t>
parse_platform_id(std::string_view text) {
  const auto bad = [](std::string message) {
    return media_library::Result<std::int64_t>::failure(
        media_library::Error::make(media_library::ErrorCode::InvalidRequest,
                                   std::move(message)));
  };

  if (text.empty()) {
    return bad("netease platform id is empty");
  }
  // 前导零会让 "007" 与 "7" 映射到同一个实体却得到不同的 ref。
  if (text.size() > 1 && text.front() == '0') {
    return bad("netease platform id must not have leading zeros");
  }

  std::int64_t value = 0;
  for (const char c : text) {
    if (!is_digit(c)) {
      return bad("netease platform id must be decimal digits only");
    }
    const int digit = c - '0';
    if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
      return bad("netease platform id overflows int64");
    }
    value = value * 10 + digit;
  }
  if (value <= 0) {
    return bad("netease platform id must be positive");
  }
  return media_library::Result<std::int64_t>::success(value);
}

std::optional<std::int64_t>
platform_id_of(const media_library::SourceRef &ref) {
  if (ref.library_id != kNetEaseLibraryId) {
    return std::nullopt;
  }
  const auto parsed = parse_platform_id(ref.entity_id);
  if (!parsed.ok()) {
    return std::nullopt;
  }
  return parsed.value();
}

media_library::Result<media_library::SourceRef>
make_ref(media_library::EntityType type, std::int64_t id) {
  if (id <= 0) {
    return media_library::Result<media_library::SourceRef>::failure(
        media_library::Error::make(media_library::ErrorCode::InvalidRequest,
                                   "netease platform id must be positive"));
  }
  media_library::SourceRef ref;
  ref.library_id = std::string(kNetEaseLibraryId);
  ref.entity_type = type;
  ref.entity_id = std::to_string(id);
  return media_library::Result<media_library::SourceRef>::success(
      std::move(ref));
}

} // namespace api_enhanced
