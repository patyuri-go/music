#include "media_library/entity_store.h"

#include "db.h"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <mutex>
#include <random>
#include <stdexcept>

namespace media_library {
namespace {

std::string random_hex(std::size_t bytes) {
  static std::mutex mutex;
  static std::mt19937_64 rng{std::random_device{}()};
  static constexpr char kHex[] = "0123456789abcdef";

  std::lock_guard<std::mutex> lock(mutex);
  std::string out;
  out.reserve(bytes * 2);
  while (out.size() < bytes * 2) {
    const std::uint64_t value = rng();
    for (int i = 0; i < 16 && out.size() < bytes * 2; ++i) {
      out.push_back(kHex[(value >> (i * 4)) & 0x0F]);
    }
  }
  return out;
}

std::string table_for(EntityType type) {
  switch (type) {
  case EntityType::Track:
    return "track";
  case EntityType::Album:
    return "album";
  case EntityType::Artist:
  case EntityType::AlbumArtist:
    return "artist";
  case EntityType::Playlist:
    return "playlist";
  }
  return "track";
}

} // namespace

EntityStore::EntityStore(std::string library_id, std::string db_path)
    : library_id_(std::move(library_id)), db_path_(std::move(db_path)) {}

std::string EntityStore::new_entity_id() { return random_hex(16); }

std::string EntityStore::internal_key(EntityType type, int internal_id) {
  return std::string(entity_type_name(type)) + '#' + std::to_string(internal_id);
}

std::string EntityStore::entity_key(EntityType type,
                                    const std::string &entity_id) {
  return std::string(entity_type_name(type)) + '#' + entity_id;
}

void EntityStore::ensure_cache() const {
  std::call_once(cache_once_, [this] {
    SQLite::Database &database = db::conn(db_path_);
    SQLite::Statement query(
        database,
        "SELECT entity_type, entity_id, internal_id FROM entity_mapping "
        "WHERE library_id = ?");
    query.bind(1, library_id_);
    while (query.executeStep()) {
      const std::string type_name = query.getColumn(0).getText();
      const std::string entity_id = query.getColumn(1).getText();
      const int internal_id = query.getColumn(2).getInt();
      by_internal_[type_name + '#' + std::to_string(internal_id)] = entity_id;
      by_entity_[type_name + '#' + entity_id] = internal_id;
    }
  });
}

std::optional<std::string>
EntityStore::find_entity_id(EntityType type, int internal_id) const {
  ensure_cache();
  const std::string key = internal_key(type, internal_id);
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = by_internal_.find(key);
  if (it == by_internal_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::string>
EntityStore::find_artist_entity_id(int internal_id) const {
  return find_entity_id(EntityType::Artist, internal_id);
}

SourceRef EntityStore::ref_for(EntityType type, int internal_id) {
  if (const auto existing = find_entity_id(type, internal_id)) {
    return SourceRef{library_id_, type, *existing};
  }

  std::string entity_id;
  if (type == EntityType::AlbumArtist) {
    // 同一底层艺术家在两种引用角色下共享 entity_id。
    if (const auto artist_id = find_artist_entity_id(internal_id)) {
      entity_id = *artist_id;
    }
  }
  if (entity_id.empty()) {
    entity_id = new_entity_id();
  }

  SQLite::Database &database = db::conn(db_path_);
  SQLite::Statement insert(
      database,
      "INSERT OR IGNORE INTO entity_mapping(library_id, entity_type, entity_id, "
      "internal_id, state, created_at, updated_at) VALUES(?, ?, ?, ?, 'active', "
      "datetime('now','localtime'), datetime('now','localtime'))");
  insert.bind(1, library_id_);
  insert.bind(2, std::string(entity_type_name(type)));
  insert.bind(3, entity_id);
  insert.bind(4, internal_id);
  insert.exec();

  // 并发插入时以先写入者为准。
  std::string stored;
  {
    SQLite::Statement query(
        database,
        "SELECT entity_id FROM entity_mapping WHERE library_id = ? AND "
        "entity_type = ? AND internal_id = ?");
    query.bind(1, library_id_);
    query.bind(2, std::string(entity_type_name(type)));
    query.bind(3, internal_id);
    if (!query.executeStep()) {
      throw std::runtime_error("failed to persist entity mapping");
    }
    stored = query.getColumn(0).getText();
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    by_internal_[internal_key(type, internal_id)] = stored;
    by_entity_[entity_key(type, stored)] = internal_id;
  }
  return SourceRef{library_id_, type, stored};
}

std::optional<SourceRef> EntityStore::find_ref(EntityType type,
                                               int internal_id) const {
  if (const auto entity_id = find_entity_id(type, internal_id)) {
    return SourceRef{library_id_, type, *entity_id};
  }
  return std::nullopt;
}

std::optional<int> EntityStore::internal_id_of(const SourceRef &ref) const {
  if (ref.library_id != library_id_) {
    return std::nullopt;
  }
  ensure_cache();
  const std::string key = entity_key(ref.entity_type, ref.entity_id);
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = by_entity_.find(key);
  if (it == by_entity_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void EntityStore::backfill() {
  SQLite::Database &database = db::conn(db_path_);
  for (const EntityType type :
       {EntityType::Artist, EntityType::Album, EntityType::Track,
        EntityType::Playlist}) {
    // 先确保 artist 映射存在，AlbumArtist 才能复用 entity_id。
    SQLite::Statement query(database,
                            "SELECT id FROM " + table_for(type) + " ORDER BY id");
    while (query.executeStep()) {
      ref_for(type, query.getColumn(0).getInt());
    }
  }

  SQLite::Statement artists(database, "SELECT id FROM artist ORDER BY id");
  while (artists.executeStep()) {
    ref_for(EntityType::AlbumArtist, artists.getColumn(0).getInt());
  }
}

} // namespace media_library
