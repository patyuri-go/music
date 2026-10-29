#ifndef MEDIA_LIBRARY_ENTITY_STORE_H
#define MEDIA_LIBRARY_ENTITY_STORE_H

#include "media_library/source_ref.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace media_library {

// 持久化 internal_id <-> entity_id 映射。
// entity_id 分配后不再改变，也不在删除实体后回收。
// 映射在内存中缓存一份，热路径不再逐条查库。
class EntityStore {
public:
  EntityStore(std::string library_id, std::string db_path);

  const std::string &library_id() const noexcept { return library_id_; }

  // 查找或分配稳定 entity_id。
  SourceRef ref_for(EntityType type, int internal_id);

  std::optional<SourceRef> find_ref(EntityType type, int internal_id) const;
  std::optional<int> internal_id_of(const SourceRef &ref) const;

  // 为现有表补齐映射；可重复调用。
  void backfill();

private:
  static std::string internal_key(EntityType type, int internal_id);
  static std::string entity_key(EntityType type, const std::string &entity_id);

  void ensure_cache() const;
  std::optional<std::string> find_entity_id(EntityType type,
                                            int internal_id) const;
  std::optional<std::string> find_artist_entity_id(int internal_id) const;
  static std::string new_entity_id();

  std::string library_id_;
  std::string db_path_;
  mutable std::once_flag cache_once_;
  mutable std::mutex mutex_;
  mutable std::unordered_map<std::string, std::string> by_internal_;
  mutable std::unordered_map<std::string, int> by_entity_;
};

} // namespace media_library

#endif // MEDIA_LIBRARY_ENTITY_STORE_H
