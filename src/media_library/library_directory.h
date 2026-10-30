#ifndef MEDIA_LIBRARY_LIBRARY_DIRECTORY_H
#define MEDIA_LIBRARY_LIBRARY_DIRECTORY_H

#include "media_library/library.h"
#include "media_library/registry.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace media_library {

// 一个库的对外可见快照。只由注册表派生，不发起任何平台请求。
struct LibraryView {
  std::string library_id;
  std::string display_name;
  PlatformKind platform = PlatformKind::Local;
  int priority = 0;
  bool enabled = true;
  HealthState health = HealthState::Ok;
  std::optional<ErrorCode> health_reason;
  std::int64_t health_checked_at = 0;
  std::vector<CapabilityId> supported;
  std::vector<CapabilityId> available;
};

// 注册表的只读视图。应用服务层用它回答“有哪些库、支持什么、健不健康”，
// 不回答“这个请求该落到哪个库”（那是 MediaRouter 的事）。
class LibraryDirectory {
public:
  explicit LibraryDirectory(std::shared_ptr<LibraryRegistry> registry);

  std::vector<LibraryView> list() const;
  std::optional<LibraryView> find(std::string_view library_id) const;

  // 参与路由且未被判定为不可用的库。
  std::vector<LibraryView> visible() const;

  std::optional<CapabilitySet> capabilities(std::string_view library_id) const;
  bool supports(std::string_view library_id, CapabilityId capability) const;
  HealthSnapshot health(std::string_view library_id) const;

private:
  std::shared_ptr<LibraryRegistry> registry_;
};

std::shared_ptr<LibraryDirectory>
make_library_directory(std::shared_ptr<LibraryRegistry> registry);

} // namespace media_library

#endif // MEDIA_LIBRARY_LIBRARY_DIRECTORY_H
