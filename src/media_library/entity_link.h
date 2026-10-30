#ifndef MEDIA_LIBRARY_ENTITY_LINK_H
#define MEDIA_LIBRARY_ENTITY_LINK_H

#include "media_library/error.h"
#include "media_library/library.h"
#include "media_library/source_ref.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace media_library {

// 5.3.3 的方面。一个方面对应一个能力，多源路由按方面分别选源。
enum class Aspect { Metadata, Lyrics, Stream, Artwork };

std::string_view aspect_name(Aspect aspect) noexcept;
std::optional<Aspect> parse_aspect(std::string_view text) noexcept;

// 方面到能力的映射（5.3.3 表格）。
CapabilityId capability_for(Aspect aspect) noexcept;

// 该方面是否接受这种实体类型。Stream/Lyrics 只对 Track 有意义；
// Artwork 接受任意 owner；Metadata 接受目录类实体。
bool aspect_accepts(Aspect aspect, EntityType type) noexcept;

// 逻辑实体标识。不是 SourceRef，也不出现在对外 API 里；
// 只表示“这些 SourceRef 指同一个逻辑实体”。
struct EntityLinkId {
  std::string value;

  bool operator==(const EntityLinkId &other) const noexcept {
    return value == other.value;
  }
  bool operator<(const EntityLinkId &other) const noexcept {
    return value < other.value;
  }
};

struct LinkedRef {
  SourceRef ref;
  std::string confidence; // isrc / mbid / manual ...
  bool verified = false;
};

// 链接级覆盖：只声明需要特判的方面。
// 稀疏语义 —— 没出现的方面必须回落全局默认，不允许当成“不参与候选”。
struct LinkOverrides {
  std::map<Aspect, std::vector<std::string>> order;

  bool empty() const noexcept { return order.empty(); }
};

// 内建兜底顺序（三级回落的第 3 级）：本地库优先，其余按注册项 priority。
// 由路由层实现并注入 resolver，这样三级回落仍然只在 effective_order 里算一次。
class BuiltinOrderSource {
public:
  virtual ~BuiltinOrderSource() = default;
  virtual std::vector<std::string> order() const = 0;
};

class EntityLinkResolver {
public:
  virtual ~EntityLinkResolver() = default;

  // 由任意一个 SourceRef 找到逻辑实体；查不到返回 NotFound，
  // 调用方据此退化为单源路由（不是错误）。
  virtual Result<EntityLinkId> link_of(const SourceRef &ref,
                                       const CallContext &ctx) const = 0;

  virtual Result<std::vector<LinkedRef>>
  refs_of(const EntityLinkId &link, const CallContext &ctx) const = 0;

  // 建链。refs 必须非空，且不得包含已属于其他链接的引用。
  virtual Status link(const std::vector<SourceRef> &refs,
                      const LinkOverrides &overrides,
                      const CallContext &ctx) = 0;

  virtual Status unlink(const SourceRef &ref, const CallContext &ctx) = 0;

  virtual Result<LinkOverrides>
  overrides_of(const EntityLinkId &link, const CallContext &ctx) const = 0;

  virtual Status set_overrides(const EntityLinkId &link,
                               const LinkOverrides &overrides,
                               const CallContext &ctx) = 0;

  virtual Status clear_overrides(const EntityLinkId &link, Aspect aspect,
                                 const CallContext &ctx) = 0;

  // 该 (link, aspect) 的最终有序库列表：
  // 链接级覆盖 -> 全局默认 -> 内建顺序，三级回落只在这里实现一次。
  // 返回值已过滤为该链接下实际存在的库。
  virtual std::vector<std::string> effective_order(const EntityLinkId &link,
                                                   Aspect aspect) const = 0;
};

// 全局默认优先级（5.3.3）。只对没有链接级覆盖的 (link, aspect) 生效。
class AspectPriority {
public:
  virtual ~AspectPriority() = default;

  virtual std::vector<std::string> order(Aspect aspect) const = 0;

  virtual Status set_order(Aspect aspect,
                           std::vector<std::string> library_ids) = 0;
};

std::shared_ptr<AspectPriority> make_memory_aspect_priority();

// 进程内内存版实现：不持久化，重启丢失。
std::shared_ptr<EntityLinkResolver>
make_memory_entity_link_resolver(std::shared_ptr<AspectPriority> defaults,
                                 std::shared_ptr<BuiltinOrderSource> builtin);

} // namespace media_library

#endif // MEDIA_LIBRARY_ENTITY_LINK_H
