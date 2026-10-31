#ifndef API_API_ENHANCED_IDS_H
#define API_API_ENHANCED_IDS_H

#include "media_library/error.h"
#include "media_library/source_ref.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace api_enhanced {

// 网易云库固定挂在 media_runtime 的这个 id 上。
inline constexpr std::string_view kNetEaseLibraryId = "netease-main";

// 解析 ref.entity_id：必须是纯十进制、无前导零、1..=INT64_MAX。
// 远端库没有「内部主键」，entity_id 直接就是平台 ID 的十进制文本。
// 解析失败返回 InvalidRequest，绝不静默截断或回退成 0。
media_library::Result<std::int64_t> parse_platform_id(std::string_view text);

// 只在 library_id 匹配、entity_id 合规时返回平台 ID；其余一律 nullopt。
std::optional<std::int64_t> platform_id_of(const media_library::SourceRef &ref);

// 构造网易云引用：to_string(ref) 形如 "netease-main:track:1969519579"。
// id 必须为正（<=0 视为调用方 bug，返回 InvalidRequest）。
media_library::Result<media_library::SourceRef>
make_ref(media_library::EntityType type, std::int64_t id);

} // namespace api_enhanced

#endif // API_API_ENHANCED_IDS_H
