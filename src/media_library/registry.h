#ifndef MEDIA_LIBRARY_REGISTRY_H
#define MEDIA_LIBRARY_REGISTRY_H

#include "media_library/error.h"
#include "media_library/library.h"
#include "media_library/source_ref.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace media_library {

// 注册项里记录的平台种类。
// Library::identity().platform 是平台自己声明的字符串；注册时做一次映射，
// 之后所有判断只看这个枚举，避免路由层到处比较字符串。
enum class PlatformKind { Local, WebDav, NetEase, Other };

std::string_view platform_kind_name(PlatformKind kind) noexcept;
PlatformKind platform_kind_from(std::string_view text) noexcept;

// 健康状态。RateLimited / Unavailable 来自 5.2.2 的注册项模型，
// 与 5.3.5 的 SourceState 一一对应。
enum class HealthState {
  Ok,
  Degraded,
  Unauthenticated,
  RateLimited,
  Unavailable
};

std::string_view health_state_name(HealthState state) noexcept;

struct HealthSnapshot {
  HealthState state = HealthState::Ok;
  std::optional<ErrorCode> reason;
  std::int64_t checked_at = 0;
};

struct LibraryLimits {
  int max_concurrency = 4;
  std::int64_t default_timeout_ms = 8000;
  int max_retries = 1;
  int circuit_failure_threshold = 5;
  std::int64_t circuit_open_ms = 30000;
};

// 注册项。capabilities / health / account 不进这个结构体：
// 能力和健康是注册表内部派生出来的状态（见 5.2.4、5.2.6），
// 放进结构体会让调用方读到过期快照。
struct LibraryRegistration {
  std::string library_id;
  std::string display_name;
  PlatformKind platform = PlatformKind::Local;
  int priority = 0;
  bool enabled = true;
  LibraryLimits limits;
};

class LibraryRegistry {
public:
  virtual ~LibraryRegistry() = default;

  virtual Status add(std::shared_ptr<Library> library,
                     const LibraryRegistration &registration) = 0;
  virtual Status remove(std::string_view library_id) = 0;

  // 返回值而不是引用：add() / remove() 会并发改表，
  // 借用内部存储会留下悬垂引用。
  virtual std::optional<LibraryRegistration>
  find(std::string_view library_id) const = 0;

  virtual std::shared_ptr<Library>
  library(std::string_view library_id) const = 0;

  // 能力查询。返回快照副本，调用方拿不到内部状态。
  virtual std::optional<CapabilitySet>
  capabilities(std::string_view library_id) const = 0;

  virtual std::vector<LibraryRegistration> list() const = 0;

  // 只做筛选与排序，不发平台请求。
  virtual std::vector<LibraryRegistration>
  candidates(CapabilityId capability, bool require_available) const = 0;

  virtual HealthSnapshot health(std::string_view library_id) const = 0;

  // 能力集合可能随账号状态变化，显式刷新（5.2.4）。
  virtual Status refresh_capabilities(std::string_view library_id) = 0;

  // 并发与熔断（5.2.6）。
  // admit 检查 enabled / stopping / 熔断窗口 / 限流窗口 / 并发上限，
  // 通过后占用一个并发名额，必须配对 release。
  virtual Status admit(std::string_view library_id, std::int64_t now_ms) = 0;
  virtual void release(std::string_view library_id,
                       const std::optional<Error> &error,
                       std::int64_t now_ms) = 0;

  virtual void shutdown() noexcept = 0;
};

std::shared_ptr<LibraryRegistry> make_library_registry();

} // namespace media_library

#endif // MEDIA_LIBRARY_REGISTRY_H
