#include "media_library/entity_link.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace media_library {

std::string_view aspect_name(Aspect aspect) noexcept {
  switch (aspect) {
  case Aspect::Metadata:
    return "metadata";
  case Aspect::Lyrics:
    return "lyrics";
  case Aspect::Stream:
    return "stream";
  case Aspect::Artwork:
    return "artwork";
  }
  return "metadata";
}

std::optional<Aspect> parse_aspect(std::string_view text) noexcept {
  if (text == "metadata") {
    return Aspect::Metadata;
  }
  if (text == "lyrics") {
    return Aspect::Lyrics;
  }
  if (text == "stream") {
    return Aspect::Stream;
  }
  if (text == "artwork") {
    return Aspect::Artwork;
  }
  return std::nullopt;
}

CapabilityId capability_for(Aspect aspect) noexcept {
  switch (aspect) {
  case Aspect::Metadata:
    return CapabilityId::CatalogRead;
  case Aspect::Lyrics:
    return CapabilityId::LyricsRead;
  case Aspect::Stream:
    return CapabilityId::StreamResolve;
  case Aspect::Artwork:
    return CapabilityId::ArtworkRead;
  }
  return CapabilityId::CatalogRead;
}

bool aspect_accepts(Aspect aspect, EntityType type) noexcept {
  switch (aspect) {
  case Aspect::Metadata:
    return type == EntityType::Track || type == EntityType::Album ||
           type == EntityType::Artist || type == EntityType::AlbumArtist ||
           type == EntityType::Playlist;
  case Aspect::Lyrics:
    return type == EntityType::Track;
  case Aspect::Stream:
    return type == EntityType::Track;
  case Aspect::Artwork:
    // 封面挂在任意 owner 上（曲目、专辑、艺术家、歌单）。
    return true;
  }
  return false;
}

namespace {

std::string ref_key(const SourceRef &ref) { return to_string(ref); }

class AspectPriorityImpl final : public AspectPriority {
public:
  std::vector<std::string> order(Aspect aspect) const override {
    std::shared_lock lock(mutex_);
    const auto it = orders_.find(aspect);
    if (it == orders_.end()) {
      return {};
    }
    return it->second;
  }

  Status set_order(Aspect aspect,
                   std::vector<std::string> library_ids) override {
    for (const std::string &id : library_ids) {
      if (id.empty()) {
        return Status::failure(Error::make(
            ErrorCode::InvalidRequest, "priority list contains empty library_id"));
      }
    }
    {
      std::unique_lock lock(mutex_);
      if (library_ids.empty()) {
        orders_.erase(aspect);
      } else {
        orders_[aspect] = std::move(library_ids);
      }
      ++revision_;
    }
    return Status::success();
  }

  // 供 resolver 与路由层判断配置是否变化。
  std::uint64_t revision() const {
    std::shared_lock lock(mutex_);
    return revision_;
  }

private:
  mutable std::shared_mutex mutex_;
  std::map<Aspect, std::vector<std::string>> orders_;
  std::uint64_t revision_ = 0;
};

class EntityLinkResolverImpl final : public EntityLinkResolver {
public:
  EntityLinkResolverImpl(std::shared_ptr<AspectPriority> defaults,
                         std::shared_ptr<BuiltinOrderSource> builtin)
      : defaults_(std::move(defaults)), builtin_(std::move(builtin)) {}

  Result<EntityLinkId>
  link_of(const SourceRef &ref, const CallContext &) const override {
    {
      std::shared_lock lock(mutex_);
      const auto it = link_of_ref_.find(ref_key(ref));
      if (it != link_of_ref_.end()) {
        return Result<EntityLinkId>::success(EntityLinkId{it->second});
      }
    }
    Error error = Error::make(ErrorCode::NotFound, "ref is not linked");
    error.library_id = ref.library_id;
    return Result<EntityLinkId>::failure(std::move(error));
  }

  Result<std::vector<LinkedRef>>
  refs_of(const EntityLinkId &link, const CallContext &) const override {
    std::shared_lock lock(mutex_);
    const auto it = links_.find(link.value);
    if (it == links_.end()) {
      Error error = Error::make(ErrorCode::NotFound, "link does not exist");
      return Result<std::vector<LinkedRef>>::failure(std::move(error));
    }
    return Result<std::vector<LinkedRef>>::success(it->second.refs);
  }

  Status link(const std::vector<SourceRef> &refs,
              const LinkOverrides &overrides,
              const CallContext &) override {
    if (refs.empty()) {
      return Status::failure(Error::make(
          ErrorCode::InvalidRequest, "cannot create a link without refs"));
    }

    // 同 link 语义必须一致；artist / album_artist 不能混。
    const EntityType type = refs.front().entity_type;
    std::unordered_set<std::string> seen;
    for (const SourceRef &ref : refs) {
      if (ref.library_id.empty() || ref.entity_id.empty()) {
        return Status::failure(Error::make(
            ErrorCode::InvalidRequest, "ref has empty library_id or entity_id"));
      }
      if (ref.entity_type != type) {
        return Status::failure(Error::make(
            ErrorCode::InvalidRequest,
            "all refs in one link must share the same entity_type"));
      }
      if (!seen.insert(ref_key(ref)).second) {
        return Status::failure(
            Error::make(ErrorCode::InvalidRequest, "duplicate ref in link"));
      }
    }

    // 覆盖只能重排同一链接内已有的库。
    std::unordered_set<std::string> libraries;
    for (const SourceRef &ref : refs) {
      libraries.insert(ref.library_id);
    }
    for (const auto &pair : overrides.order) {
      for (const std::string &id : pair.second) {
        if (libraries.count(id) == 0) {
          return Status::failure(Error::make(
              ErrorCode::InvalidRequest,
              "override names a library that is not part of this link: " + id));
        }
      }
    }

    std::unique_lock lock(mutex_);
    for (const SourceRef &ref : refs) {
      const auto it = link_of_ref_.find(ref_key(ref));
      if (it != link_of_ref_.end()) {
        return Status::failure(Error::make(
            ErrorCode::Conflict, "ref already belongs to another link"));
      }
    }

    Link link_record;
    link_record.id = "link-" + std::to_string(++next_id_);
    for (const SourceRef &ref : refs) {
      LinkedRef linked;
      linked.ref = ref;
      linked.confidence = "manual";
      linked.verified = true;
      link_record.refs.push_back(std::move(linked));
      link_of_ref_[ref_key(ref)] = link_record.id;
    }
    link_record.overrides = overrides;
    links_.emplace(link_record.id, std::move(link_record));
    return Status::success();
  }

  Status unlink(const SourceRef &ref, const CallContext &) override {
    std::unique_lock lock(mutex_);
    const auto it = link_of_ref_.find(ref_key(ref));
    if (it == link_of_ref_.end()) {
      return Status::failure(
          Error::make(ErrorCode::NotFound, "ref is not linked"));
    }
    const std::string link_id = it->second;
    link_of_ref_.erase(it);

    const auto link_it = links_.find(link_id);
    if (link_it == links_.end()) {
      return Status::success();
    }
    auto &refs = link_it->second.refs;
    refs.erase(std::remove_if(refs.begin(), refs.end(),
                              [&ref](const LinkedRef &item) {
                                return item.ref == ref;
                              }),
               refs.end());
    // 链接级覆盖的生命周期跟随 link：成员空了就整体丢弃，
    // 覆盖也随之失效（5.3.3「覆盖生命周期跟随 link」）。
    if (refs.size() < 2) {
      if (!refs.empty()) {
        link_of_ref_.erase(ref_key(refs.front().ref));
      }
      links_.erase(link_it);
    }
    return Status::success();
  }

  Result<LinkOverrides>
  overrides_of(const EntityLinkId &link, const CallContext &) const override {
    std::shared_lock lock(mutex_);
    const auto it = links_.find(link.value);
    if (it == links_.end()) {
      Error error = Error::make(ErrorCode::NotFound, "link does not exist");
      return Result<LinkOverrides>::failure(std::move(error));
    }
    return Result<LinkOverrides>::success(it->second.overrides);
  }

  Status set_overrides(const EntityLinkId &link, const LinkOverrides &overrides,
                       const CallContext &) override {
    std::unique_lock lock(mutex_);
    const auto it = links_.find(link.value);
    if (it == links_.end()) {
      return Status::failure(
          Error::make(ErrorCode::NotFound, "link does not exist"));
    }
    std::unordered_set<std::string> libraries;
    for (const LinkedRef &item : it->second.refs) {
      libraries.insert(item.ref.library_id);
    }
    for (const auto &pair : overrides.order) {
      for (const std::string &id : pair.second) {
        if (libraries.count(id) == 0) {
          return Status::failure(Error::make(
              ErrorCode::InvalidRequest,
              "override names a library that is not part of this link: " + id));
        }
      }
    }
    it->second.overrides = overrides;
    return Status::success();
  }

  Status clear_overrides(const EntityLinkId &link, Aspect aspect,
                         const CallContext &) override {
    std::unique_lock lock(mutex_);
    const auto it = links_.find(link.value);
    if (it == links_.end()) {
      return Status::failure(
          Error::make(ErrorCode::NotFound, "link does not exist"));
    }
    it->second.overrides.order.erase(aspect);
    return Status::success();
  }

  std::vector<std::string> effective_order(const EntityLinkId &link,
                                           Aspect aspect) const override {
    std::shared_lock lock(mutex_);
    const auto it = links_.find(link.value);
    if (it == links_.end()) {
      return {};
    }

    // 该链接实际存在的库，按库 ID 收集。
    std::vector<std::string> available;
    for (const LinkedRef &item : it->second.refs) {
      if (std::find(available.begin(), available.end(),
                    item.ref.library_id) == available.end()) {
        available.push_back(item.ref.library_id);
      }
    }

    // 三级回落：链接覆盖 -> 全局默认 -> 内建顺序。
    // 每级都拷贝成值，避免把指针指向容器内部元素。
    std::vector<std::string> preferred;
    bool has_preferred = false;

    const auto override_it = it->second.overrides.order.find(aspect);
    if (override_it != it->second.overrides.order.end()) {
      preferred = override_it->second;
      has_preferred = !preferred.empty();
    }

    if (!has_preferred && defaults_ != nullptr) {
      preferred = defaults_->order(aspect);
      has_preferred = !preferred.empty();
    }

    if (!has_preferred && builtin_ != nullptr) {
      preferred = builtin_->order();
      has_preferred = !preferred.empty();
    }

    // 按 preferred 排序，只保留该链接实际存在的库（不能引入 link 外来源）。
    std::vector<std::string> result;
    std::unordered_set<std::string> emitted;
    if (has_preferred) {
      for (const std::string &id : preferred) {
        if (emitted.count(id) != 0) {
          continue;
        }
        if (std::find(available.begin(), available.end(), id) ==
            available.end()) {
          continue;
        }
        emitted.insert(id);
        result.push_back(id);
      }
    }
    // 未列出的库追加末尾，仍然可候选（5.3.3）。
    for (const std::string &id : available) {
      if (emitted.count(id) == 0) {
        emitted.insert(id);
        result.push_back(id);
      }
    }
    return result;
  }

private:
  struct Link {
    std::string id;
    std::vector<LinkedRef> refs;
    LinkOverrides overrides;
  };

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, Link> links_;
  std::unordered_map<std::string, std::string> link_of_ref_;
  std::shared_ptr<AspectPriority> defaults_;
  std::shared_ptr<BuiltinOrderSource> builtin_;
  int next_id_ = 0;
};

} // namespace

std::shared_ptr<AspectPriority> make_memory_aspect_priority() {
  return std::make_shared<AspectPriorityImpl>();
}

std::shared_ptr<EntityLinkResolver>
make_memory_entity_link_resolver(std::shared_ptr<AspectPriority> defaults,
                                 std::shared_ptr<BuiltinOrderSource> builtin) {
  return std::make_shared<EntityLinkResolverImpl>(std::move(defaults),
                                                  std::move(builtin));
}

} // namespace media_library
