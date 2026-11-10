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

// 链接级优先级只覆盖“取某一个实体的某个方面”的场景。
// search 不进入链接 override，它由全局 AggregatePriority 负责。
enum class Aspect { Metadata, Lyrics, Stream, Artwork };

std::string_view aspect_name(Aspect aspect) noexcept;
std::optional<Aspect> parse_aspect(std::string_view text) noexcept;
CapabilityId capability_for(Aspect aspect) noexcept;
bool aspect_accepts(Aspect aspect, EntityType type) noexcept;

struct LinkedRef {
  SourceRef ref;
  std::string confidence;
  bool verified = false;
};

struct LinkOverrides {
  std::map<Aspect, std::vector<std::string>> order;

  bool empty() const noexcept { return order.empty(); }
};

// 一个链接的完整视图。链接没有对外 ID，调用方用任意成员 SourceRef 定位它。
struct EntityLinkView {
  EntityType entity_type = EntityType::Track;
  std::vector<LinkedRef> refs;
  LinkOverrides overrides;
  std::string created_at;
  std::string updated_at;
};

// 列表项。anchor 是链接的第一个成员，可作为后续操作的句柄。
struct EntityLinkSummary {
  SourceRef anchor;
  EntityType entity_type = EntityType::Track;
  int member_count = 0;
  std::string created_at;
  std::string updated_at;
};

class BuiltinOrderSource {
public:
  virtual ~BuiltinOrderSource() = default;
  virtual std::vector<std::string> order() const = 0;
};

// 跨库 EntityLink 的存储接口。所有链接句柄都是 SourceRef：
// 任意一个成员的 SourceRef 都可以定位它所属的链接。
// 接口不接受、不返回内部 link_id。
class EntityLinkResolver {
public:
  virtual ~EntityLinkResolver() = default;

  // 由任意成员 SourceRef 找到链接的完整视图。
  // 查不到返回 NotFound，调用方据此退化为单源路由。
  virtual Result<EntityLinkView> find_link(const SourceRef &ref,
                                           const CallContext &ctx) const = 0;

  // 按成员加入顺序返回链接内的全部 ref。
  virtual Result<std::vector<LinkedRef>>
  refs_of(const SourceRef &ref, const CallContext &ctx) const = 0;

  // 返回链接级 override；没有 override 时返回空 LinkOverrides。
  virtual Result<LinkOverrides>
  overrides_of(const SourceRef &ref, const CallContext &ctx) const = 0;

  // 分页列出链接；type 为空表示不过滤。
  virtual Result<std::vector<EntityLinkSummary>>
  list_links(std::optional<EntityType> type, int limit, int offset,
             const CallContext &ctx) const = 0;

  // 统一入口：确保 a、b 属于同一个链接，调用方不需要区分创建和追加。
  // - 两个 ref 都未链接：新建链接，成员为 a、b。
  // - 其中一个已链接：把另一个追加进该链接。
  // - 两个已在同一链接：幂等成功。
  // - 两个分属不同链接：Conflict。
  // 两个 ref 必须 entity_type 相同、library_id 不同（一个链接每库最多一个成员）。
  virtual Status link(const SourceRef &a, const SourceRef &b,
                      const CallContext &ctx) = 0;

  // 按 ref 移除成员；剩余少于 2 个时删除整个链接。
  virtual Status remove_ref(const SourceRef &ref,
                            const CallContext &ctx) = 0;

  // 删除 anchor 所属的整个链接，成员和 override 级联删除。
  virtual Status delete_link(const SourceRef &anchor,
                             const CallContext &ctx) = 0;

  // 全量替换 anchor 所属链接的 override。
  virtual Status set_overrides(const SourceRef &anchor,
                               const LinkOverrides &overrides,
                               const CallContext &ctx) = 0;

  // 只清除 anchor 所属链接指定 aspect 的 override。
  virtual Status clear_overrides(const SourceRef &anchor, Aspect aspect,
                                 const CallContext &ctx) = 0;

  // 先取链接 override，再取全局默认，最后取内建顺序。
  // 链接不存在时返回空 vector。
  virtual std::vector<std::string> effective_order(const SourceRef &anchor,
                                                   Aspect aspect) const = 0;
};

class AspectPriority {
public:
  virtual ~AspectPriority() = default;
  virtual std::vector<std::string> order(Aspect aspect) const = 0;
  virtual Status set_order(Aspect aspect,
                           std::vector<std::string> library_ids) = 0;
};

std::shared_ptr<AspectPriority> make_memory_aspect_priority();

std::shared_ptr<EntityLinkResolver> make_entity_link_resolver(
    std::shared_ptr<AspectPriority> defaults,
    std::shared_ptr<BuiltinOrderSource> builtin);

} // namespace media_library

#endif // MEDIA_LIBRARY_ENTITY_LINK_H
