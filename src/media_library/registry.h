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
  // 注册时的初始健康状态。未登录/已知不可用时不必先 add 成 Ok 再被修正，
  // 否则第一个请求会按 Ok 的顺序被打到注定失败的库上。
  HealthState initial_health = HealthState::Ok;
  // 初始健康的原因（如未登录 = AuthRequired），仅在非 Ok 时有意义。
  std::optional<ErrorCode> initial_health_reason;
};

class LibraryRegistry;

// 借用一次操作的凭证（5.2.6）。
//
// acquire() 成功即占用该库的一个并发名额，本对象析构时必定归还——
// 「admit / release 必须配对」从纪律变成类型约束。
//
// 正常路径要用 finish(error) 显式交出本次调用的结果：release 的第二个参数
// 不是备注，而是健康状态机的输入（nullopt 会把 consecutive_failures 清零
// 并清掉已打开的熔断）。析构兜底走 Cancelled：只归还名额，不碰健康状态。
class LibraryLease {
public:
  LibraryLease() = default;
  LibraryLease(std::shared_ptr<LibraryRegistry> registry, std::string library_id,
               std::shared_ptr<Library> library);
  ~LibraryLease();

  LibraryLease(LibraryLease &&other) noexcept;
  LibraryLease &operator=(LibraryLease &&other) noexcept;
  LibraryLease(const LibraryLease &) = delete;
  LibraryLease &operator=(const LibraryLease &) = delete;

  // 本次借到的库实例；未持有时为空。
  const std::shared_ptr<Library> &library() const { return library_; }
  bool holds() const noexcept { return registry_ != nullptr; }

  // 交出本次调用的结果并归还名额。幂等，可重复调用。
  void finish(const std::optional<Error> &error);

private:
  friend class LibraryRegistry;

  void release_with(const std::optional<Error> &error);

  std::shared_ptr<LibraryRegistry> registry_;
  std::string library_id_;
  std::shared_ptr<Library> library_;
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

  // 借一个库来做一次操作：查表 → 取实例 → 验能力 → 占并发名额（5.2.6）。
  // 成功返回的 LibraryLease 已经占用名额，析构即归还。
  // 判断「有没有资格」全部在注册表内部，调用方不做也不应重复这套顺序。
  virtual Result<LibraryLease> acquire(std::string_view library_id,
                                       CapabilityId capability,
                                       std::int64_t now_ms) = 0;

  // 能力集合可能随账号状态变化，显式刷新（5.2.4）。
  virtual Status refresh_capabilities(std::string_view library_id) = 0;

  virtual void shutdown() noexcept = 0;

private:
  friend class LibraryLease;

  // 并发与熔断（5.2.6）。
  // admit 检查 enabled / stopping / 熔断窗口 / 限流窗口 / 并发上限，
  // 通过后占用一个并发名额，必须配对 release——用 acquire() 拿 LibraryLease，
  // 不要直接调这两个：外部调用顺序写错会泄漏名额或误清熔断。
  virtual Status admit(std::string_view library_id, std::int64_t now_ms) = 0;
  virtual void release(std::string_view library_id,
                       const std::optional<Error> &error,
                       std::int64_t now_ms) = 0;
};

std::shared_ptr<LibraryRegistry> make_library_registry();

} // namespace media_library

#endif // MEDIA_LIBRARY_REGISTRY_H
