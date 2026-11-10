#include "media_library/entity_link.h"

#include "db.h"
#include "schema.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <utility>

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

struct LocatedLink {
  std::int64_t link_id = 0;
  EntityType entity_type = EntityType::Track;
};

bool valid_ref(const SourceRef &ref) {
  return !ref.library_id.empty() && !ref.entity_id.empty();
}

std::string aspect_text(Aspect aspect) {
  return std::string(aspect_name(aspect));
}

std::string entity_type_text(EntityType type) {
  return std::string(entity_type_name(type));
}

// ---- 结果集取值助手 -------------------------------------------------------

const db::Cell &cell_at(const db::Row &row, std::size_t index) {
  static const db::Cell kNull{db::Null{}};
  return index < row.size() ? row[index] : kNull;
}

bool is_null_at(const db::Row &row, std::size_t index) {
  return cell_at(row, index).is_null();
}

std::int64_t int_at(const db::Row &row, std::size_t index) {
  const db::Cell &cell = cell_at(row, index);
  if (const auto *value = std::get_if<std::int64_t>(&cell.value)) {
    return *value;
  }
  if (const auto *value = std::get_if<double>(&cell.value)) {
    return static_cast<std::int64_t>(*value);
  }
  if (const auto *value = std::get_if<std::string>(&cell.value)) {
    try {
      return std::stoll(*value);
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

std::string text_at(const db::Row &row, std::size_t index) {
  const db::Cell &cell = cell_at(row, index);
  if (const auto *value = std::get_if<std::string>(&cell.value)) {
    return *value;
  }
  if (const auto *value = std::get_if<std::int64_t>(&cell.value)) {
    return std::to_string(*value);
  }
  if (const auto *value = std::get_if<double>(&cell.value)) {
    return std::to_string(*value);
  }
  return {};
}

// ---- 错误映射 -------------------------------------------------------------

Error db_error(const db::Error &error, const char *context) {
  const bool conflict = error.kind() == db::ErrorKind::Constraint ||
                        error.kind() == db::ErrorKind::NotNull;
  return Error::make(conflict ? ErrorCode::Conflict : ErrorCode::Internal,
                     std::string(context) + ": " + error.what());
}

Error internal_error(const std::exception &error, const char *context) {
  return Error::make(ErrorCode::Internal,
                     std::string(context) + ": " + error.what());
}

// ---- 数据访问 -------------------------------------------------------------

std::optional<LocatedLink> locate_link(db::Session &database,
                                       const SourceRef &ref) {
  const auto result = database.query(
      "SELECT l.link_id, l.entity_type "
      "FROM entity_link_member m "
      "JOIN entity_link l ON l.link_id = m.link_id "
      "WHERE m.library_id = ? AND m.entity_type = ? AND m.entity_id = ?",
      {ref.library_id, entity_type_text(ref.entity_type), ref.entity_id});
  if (result.rows.empty()) {
    return std::nullopt;
  }
  const auto &row = result.rows.front();
  const std::optional<EntityType> type = parse_entity_type(text_at(row, 1));
  if (!type.has_value()) {
    return std::nullopt;
  }
  LocatedLink located;
  located.link_id = int_at(row, 0);
  located.entity_type = *type;
  return located;
}

std::vector<std::string> member_libraries(db::Session &database,
                                          std::int64_t link_id) {
  const auto result = database.query(
      "SELECT library_id FROM entity_link_member "
      "WHERE link_id = ? ORDER BY member_id",
      {link_id});
  std::vector<std::string> libraries;
  libraries.reserve(result.rows.size());
  for (const auto &row : result.rows) {
    libraries.push_back(text_at(row, 0));
  }
  return libraries;
}

LinkOverrides load_overrides(db::Session &database, std::int64_t link_id) {
  const auto result = database.query(
      "SELECT aspect, library_id FROM entity_link_override "
      "WHERE link_id = ? ORDER BY aspect, position",
      {link_id});
  LinkOverrides overrides;
  for (const auto &row : result.rows) {
    const std::optional<Aspect> aspect = parse_aspect(text_at(row, 0));
    if (!aspect.has_value()) {
      continue;
    }
    overrides.order[*aspect].push_back(text_at(row, 1));
  }
  return overrides;
}

std::optional<Error>
validate_overrides(const LinkOverrides &overrides,
                   const std::unordered_set<std::string> &member_libraries) {
  for (const auto &entry : overrides.order) {
    std::unordered_set<std::string> seen;
    for (const std::string &library_id : entry.second) {
      if (library_id.empty()) {
        return Error::make(ErrorCode::InvalidRequest,
                           "override contains empty library_id");
      }
      if (!seen.insert(library_id).second) {
        return Error::make(ErrorCode::InvalidRequest,
                           "override contains duplicate library_id");
      }
      if (member_libraries.count(library_id) == 0) {
        return Error::make(ErrorCode::InvalidRequest,
                           "override references non-member library");
      }
    }
  }
  return std::nullopt;
}

void insert_overrides(db::Session &database, std::int64_t link_id,
                      const LinkOverrides &overrides) {
  for (const auto &entry : overrides.order) {
    std::int64_t position = 0;
    for (const std::string &library_id : entry.second) {
      database.execute(
          "INSERT INTO entity_link_override"
          "(link_id, aspect, library_id, position) VALUES(?, ?, ?, ?)",
          {link_id, aspect_text(entry.first), library_id, position});
      ++position;
    }
  }
}

void touch_link(db::Session &database, std::int64_t link_id) {
  database.execute("UPDATE entity_link SET updated_at = " +
                       schema::sql::nowExpr(database.dialect()) +
                       " WHERE link_id = ?",
                   {link_id});
}

std::int64_t insert_link_row(db::Session &database, EntityType entity_type) {
  database.execute("INSERT INTO entity_link(entity_type) VALUES(?)",
                   {entity_type_text(entity_type)});
  return database.lastInsertId();
}

void insert_member(db::Session &database, std::int64_t link_id,
                   const LinkedRef &item) {
  database.execute(
      "INSERT INTO entity_link_member"
      "(link_id, library_id, entity_type, entity_id, confidence, verified) "
      "VALUES(?, ?, ?, ?, ?, ?)",
      {link_id, item.ref.library_id, entity_type_text(item.ref.entity_type),
       item.ref.entity_id,
       item.confidence.empty() ? std::string("manual") : item.confidence,
       static_cast<std::int64_t>(item.verified ? 1 : 0)});
}

bool link_has_library(db::Session &database, std::int64_t link_id,
                      const std::string &library_id) {
  const auto result = database.query(
      "SELECT 1 FROM entity_link_member WHERE link_id = ? AND library_id = ?",
      {link_id, library_id});
  return !result.rows.empty();
}

class EntityLinkResolverImpl final : public EntityLinkResolver {
public:
  EntityLinkResolverImpl(std::shared_ptr<AspectPriority> defaults,
                         std::shared_ptr<BuiltinOrderSource> builtin)
      : defaults_(std::move(defaults)), builtin_(std::move(builtin)) {}

  Result<EntityLinkView> find_link(const SourceRef &ref,
                                   const CallContext &) const override {
    try {
      // session 是 thread_local，每次现取；不能跨线程缓存。
      db::Session &database = db::mainSession();
      db::Transaction transaction(database, db::TransactionMode::Deferred);
      const std::optional<LocatedLink> located = locate_link(database, ref);
      if (!located.has_value()) {
        return Result<EntityLinkView>::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }

      EntityLinkView view;
      view.entity_type = located->entity_type;

      const auto head = database.query(
          "SELECT created_at, updated_at FROM entity_link WHERE link_id = ?",
          {located->link_id});
      if (head.rows.empty()) {
        return Result<EntityLinkView>::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }
      view.created_at = text_at(head.rows.front(), 0);
      if (!is_null_at(head.rows.front(), 1)) {
        view.updated_at = text_at(head.rows.front(), 1);
      }

      const auto members = database.query(
          "SELECT library_id, entity_type, entity_id, confidence, verified "
          "FROM entity_link_member WHERE link_id = ? ORDER BY member_id",
          {located->link_id});
      for (const auto &row : members.rows) {
        const std::optional<EntityType> type =
            parse_entity_type(text_at(row, 1));
        if (!type.has_value()) {
          return Result<EntityLinkView>::failure(Error::make(
              ErrorCode::Internal, "invalid entity_type in entity_link_member"));
        }
        LinkedRef item;
        item.ref.library_id = text_at(row, 0);
        item.ref.entity_type = *type;
        item.ref.entity_id = text_at(row, 2);
        item.confidence = text_at(row, 3);
        item.verified = int_at(row, 4) != 0;
        view.refs.push_back(std::move(item));
      }

      view.overrides = load_overrides(database, located->link_id);
      transaction.commit();
      return Result<EntityLinkView>::success(std::move(view));
    } catch (const db::Error &error) {
      return Result<EntityLinkView>::failure(
          db_error(error, "find_link failed"));
    } catch (const std::exception &error) {
      return Result<EntityLinkView>::failure(
          internal_error(error, "find_link failed"));
    }
  }

  Result<std::vector<LinkedRef>>
  refs_of(const SourceRef &ref, const CallContext &) const override {
    try {
      db::Session &database = db::mainSession();
      const std::optional<LocatedLink> located = locate_link(database, ref);
      if (!located.has_value()) {
        return Result<std::vector<LinkedRef>>::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }

      const auto result = database.query(
          "SELECT library_id, entity_type, entity_id, confidence, verified "
          "FROM entity_link_member WHERE link_id = ? ORDER BY member_id",
          {located->link_id});
      std::vector<LinkedRef> refs;
      refs.reserve(result.rows.size());
      for (const auto &row : result.rows) {
        const std::optional<EntityType> type =
            parse_entity_type(text_at(row, 1));
        if (!type.has_value()) {
          return Result<std::vector<LinkedRef>>::failure(Error::make(
              ErrorCode::Internal, "invalid entity_type in entity_link_member"));
        }
        LinkedRef item;
        item.ref.library_id = text_at(row, 0);
        item.ref.entity_type = *type;
        item.ref.entity_id = text_at(row, 2);
        item.confidence = text_at(row, 3);
        item.verified = int_at(row, 4) != 0;
        refs.push_back(std::move(item));
      }
      return Result<std::vector<LinkedRef>>::success(std::move(refs));
    } catch (const db::Error &error) {
      return Result<std::vector<LinkedRef>>::failure(
          db_error(error, "refs_of failed"));
    } catch (const std::exception &error) {
      return Result<std::vector<LinkedRef>>::failure(
          internal_error(error, "refs_of failed"));
    }
  }

  Result<LinkOverrides>
  overrides_of(const SourceRef &ref, const CallContext &) const override {
    try {
      db::Session &database = db::mainSession();
      const std::optional<LocatedLink> located = locate_link(database, ref);
      if (!located.has_value()) {
        return Result<LinkOverrides>::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }
      return Result<LinkOverrides>::success(
          load_overrides(database, located->link_id));
    } catch (const db::Error &error) {
      return Result<LinkOverrides>::failure(
          db_error(error, "overrides_of failed"));
    } catch (const std::exception &error) {
      return Result<LinkOverrides>::failure(
          internal_error(error, "overrides_of failed"));
    }
  }

  Result<std::vector<EntityLinkSummary>>
  list_links(std::optional<EntityType> type, int limit, int offset,
             const CallContext &) const override {
    if (limit < 1 || limit > 200 || offset < 0) {
      return Result<std::vector<EntityLinkSummary>>::failure(Error::make(
          ErrorCode::InvalidRequest, "invalid list_links limit or offset"));
    }
    try {
      db::Session &database = db::mainSession();
      const char *base_sql =
          "SELECT l.link_id, l.entity_type, l.created_at, l.updated_at, "
          "(SELECT COUNT(*) FROM entity_link_member c "
          " WHERE c.link_id = l.link_id) AS member_count, "
          "m.library_id, m.entity_type, m.entity_id "
          "FROM entity_link l "
          "JOIN entity_link_member m ON m.member_id = ("
          "  SELECT MIN(x.member_id) FROM entity_link_member x "
          "  WHERE x.link_id = l.link_id) ";
      const std::string sql =
          type.has_value()
              ? std::string(base_sql) +
                    "WHERE l.entity_type = ? ORDER BY l.link_id DESC "
                    "LIMIT ? OFFSET ?"
              : std::string(base_sql) +
                    "ORDER BY l.link_id DESC LIMIT ? OFFSET ?";

      db::ParamList params;
      params.reserve(3);
      if (type.has_value()) {
        params.emplace_back(entity_type_text(*type));
      }
      params.emplace_back(static_cast<std::int64_t>(limit));
      params.emplace_back(static_cast<std::int64_t>(offset));

      const auto result = database.query(sql, params);
      std::vector<EntityLinkSummary> summaries;
      summaries.reserve(result.rows.size());
      for (const auto &row : result.rows) {
        const std::optional<EntityType> link_type =
            parse_entity_type(text_at(row, 1));
        const std::optional<EntityType> anchor_type =
            parse_entity_type(text_at(row, 6));
        if (!link_type.has_value() || !anchor_type.has_value()) {
          return Result<std::vector<EntityLinkSummary>>::failure(
              Error::make(ErrorCode::Internal, "invalid entity_type in link"));
        }
        EntityLinkSummary summary;
        summary.anchor.library_id = text_at(row, 5);
        summary.anchor.entity_type = *anchor_type;
        summary.anchor.entity_id = text_at(row, 7);
        summary.entity_type = *link_type;
        summary.member_count = static_cast<int>(int_at(row, 4));
        summary.created_at = text_at(row, 2);
        if (!is_null_at(row, 3)) {
          summary.updated_at = text_at(row, 3);
        }
        summaries.push_back(std::move(summary));
      }
      return Result<std::vector<EntityLinkSummary>>::success(
          std::move(summaries));
    } catch (const db::Error &error) {
      return Result<std::vector<EntityLinkSummary>>::failure(
          db_error(error, "list_links failed"));
    } catch (const std::exception &error) {
      return Result<std::vector<EntityLinkSummary>>::failure(
          internal_error(error, "list_links failed"));
    }
  }

  Status link(const SourceRef &a, const SourceRef &b,
              const CallContext &) override {
    if (!valid_ref(a) || !valid_ref(b)) {
      return Status::failure(Error::make(
          ErrorCode::InvalidRequest, "ref contains empty library_id or entity_id"));
    }
    if (a == b) {
      return Status::failure(Error::make(
          ErrorCode::InvalidRequest, "link requires two different refs"));
    }
    if (a.entity_type != b.entity_type) {
      return Status::failure(Error::make(
          ErrorCode::InvalidRequest, "link refs have mixed entity_type"));
    }
    if (a.library_id == b.library_id) {
      return Status::failure(Error::make(
          ErrorCode::InvalidRequest,
          "link requires refs from different libraries"));
    }
    try {
      db::Session &database = db::mainSession();
      db::Transaction transaction(database, db::TransactionMode::Immediate);

      const std::optional<LocatedLink> link_a = locate_link(database, a);
      const std::optional<LocatedLink> link_b = locate_link(database, b);

      if (link_a.has_value() && link_b.has_value()) {
        if (link_a->link_id != link_b->link_id) {
          return Status::failure(Error::make(
              ErrorCode::Conflict, "refs belong to different links"));
        }
        transaction.commit();
        return Status::success();
      }

      if (!link_a.has_value() && !link_b.has_value()) {
        const std::int64_t link_id = insert_link_row(database, a.entity_type);
        insert_member(database, link_id, LinkedRef{a, "", false});
        insert_member(database, link_id, LinkedRef{b, "", false});
        transaction.commit();
        return Status::success();
      }

      const LocatedLink &located = link_a.has_value() ? *link_a : *link_b;
      const SourceRef &incoming = link_a.has_value() ? b : a;
      if (link_has_library(database, located.link_id, incoming.library_id)) {
        return Status::failure(Error::make(
            ErrorCode::Conflict, "link already has a member from this library"));
      }
      insert_member(database, located.link_id, LinkedRef{incoming, "", false});
      touch_link(database, located.link_id);
      transaction.commit();
      return Status::success();
    } catch (const db::Error &error) {
      return Status::failure(db_error(error, "link failed"));
    } catch (const std::exception &error) {
      return Status::failure(internal_error(error, "link failed"));
    }
  }

  Status remove_ref(const SourceRef &ref, const CallContext &) override {
    if (!valid_ref(ref)) {
      return Status::failure(Error::make(
          ErrorCode::InvalidRequest, "ref contains empty library_id or entity_id"));
    }
    try {
      db::Session &database = db::mainSession();
      db::Transaction transaction(database, db::TransactionMode::Immediate);
      const std::optional<LocatedLink> located = locate_link(database, ref);
      if (!located.has_value()) {
        return Status::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }

      const std::int64_t removed = database.execute(
          "DELETE FROM entity_link_member "
          "WHERE library_id = ? AND entity_type = ? AND entity_id = ?",
          {ref.library_id, entity_type_text(ref.entity_type), ref.entity_id});
      if (removed == 0) {
        return Status::failure(
            Error::make(ErrorCode::NotFound, "entity link member not found"));
      }

      const auto counted = database.query(
          "SELECT COUNT(*) FROM entity_link_member WHERE link_id = ?",
          {located->link_id});
      if (counted.rows.empty()) {
        return Status::failure(
            Error::make(ErrorCode::Internal, "failed to count link members"));
      }
      const std::int64_t remaining = int_at(counted.rows.front(), 0);

      if (remaining < 2) {
        database.execute("DELETE FROM entity_link WHERE link_id = ?",
                         {located->link_id});
      } else {
        touch_link(database, located->link_id);
      }

      transaction.commit();
      return Status::success();
    } catch (const db::Error &error) {
      return Status::failure(db_error(error, "remove_ref failed"));
    } catch (const std::exception &error) {
      return Status::failure(internal_error(error, "remove_ref failed"));
    }
  }

  Status delete_link(const SourceRef &anchor, const CallContext &) override {
    if (!valid_ref(anchor)) {
      return Status::failure(Error::make(
          ErrorCode::InvalidRequest, "ref contains empty library_id or entity_id"));
    }
    try {
      db::Session &database = db::mainSession();
      db::Transaction transaction(database, db::TransactionMode::Immediate);
      const std::optional<LocatedLink> located = locate_link(database, anchor);
      if (!located.has_value()) {
        return Status::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }
      database.execute("DELETE FROM entity_link WHERE link_id = ?",
                       {located->link_id});
      transaction.commit();
      return Status::success();
    } catch (const db::Error &error) {
      return Status::failure(db_error(error, "delete_link failed"));
    } catch (const std::exception &error) {
      return Status::failure(internal_error(error, "delete_link failed"));
    }
  }

  Status set_overrides(const SourceRef &anchor,
                       const LinkOverrides &overrides,
                       const CallContext &) override {
    if (!valid_ref(anchor)) {
      return Status::failure(Error::make(
          ErrorCode::InvalidRequest, "ref contains empty library_id or entity_id"));
    }
    try {
      db::Session &database = db::mainSession();
      db::Transaction transaction(database, db::TransactionMode::Immediate);
      const std::optional<LocatedLink> located = locate_link(database, anchor);
      if (!located.has_value()) {
        return Status::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }

      const std::vector<std::string> libraries =
          member_libraries(database, located->link_id);
      const std::unordered_set<std::string> member_set(libraries.begin(),
                                                       libraries.end());
      if (const std::optional<Error> error =
              validate_overrides(overrides, member_set);
          error.has_value()) {
        return Status::failure(*error);
      }

      database.execute("DELETE FROM entity_link_override WHERE link_id = ?",
                       {located->link_id});

      insert_overrides(database, located->link_id, overrides);
      touch_link(database, located->link_id);
      transaction.commit();
      return Status::success();
    } catch (const db::Error &error) {
      return Status::failure(db_error(error, "set_overrides failed"));
    } catch (const std::exception &error) {
      return Status::failure(internal_error(error, "set_overrides failed"));
    }
  }

  Status clear_overrides(const SourceRef &anchor, Aspect aspect,
                         const CallContext &) override {
    if (!valid_ref(anchor)) {
      return Status::failure(Error::make(
          ErrorCode::InvalidRequest, "ref contains empty library_id or entity_id"));
    }
    try {
      db::Session &database = db::mainSession();
      db::Transaction transaction(database, db::TransactionMode::Immediate);
      const std::optional<LocatedLink> located = locate_link(database, anchor);
      if (!located.has_value()) {
        return Status::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }

      database.execute(
          "DELETE FROM entity_link_override WHERE link_id = ? AND aspect = ?",
          {located->link_id, aspect_text(aspect)});
      touch_link(database, located->link_id);
      transaction.commit();
      return Status::success();
    } catch (const db::Error &error) {
      return Status::failure(db_error(error, "clear_overrides failed"));
    } catch (const std::exception &error) {
      return Status::failure(internal_error(error, "clear_overrides failed"));
    }
  }

  std::vector<std::string> effective_order(const SourceRef &anchor,
                                           Aspect aspect) const override {
    try {
      db::Session &database = db::mainSession();
      const std::optional<LocatedLink> located = locate_link(database, anchor);
      if (!located.has_value()) {
        return {};
      }

      const std::vector<std::string> available =
          member_libraries(database, located->link_id);
      if (available.empty()) {
        return {};
      }

      std::vector<std::string> preferred;
      const auto result = database.query(
          "SELECT library_id FROM entity_link_override "
          "WHERE link_id = ? AND aspect = ? ORDER BY position",
          {located->link_id, aspect_text(aspect)});
      preferred.reserve(result.rows.size());
      for (const auto &row : result.rows) {
        preferred.push_back(text_at(row, 0));
      }

      if (preferred.empty() && defaults_ != nullptr) {
        preferred = defaults_->order(aspect);
      }
      if (preferred.empty() && builtin_ != nullptr) {
        preferred = builtin_->order();
      }

      const std::unordered_set<std::string> available_set(available.begin(),
                                                          available.end());
      std::unordered_set<std::string> seen;
      std::vector<std::string> ordered;
      ordered.reserve(available.size());
      for (const std::string &library_id : preferred) {
        if (available_set.count(library_id) == 0) {
          continue;
        }
        if (seen.insert(library_id).second) {
          ordered.push_back(library_id);
        }
      }
      for (const std::string &library_id : available) {
        if (seen.insert(library_id).second) {
          ordered.push_back(library_id);
        }
      }
      return ordered;
    } catch (const db::Error &) {
      return {};
    } catch (const std::exception &) {
      return {};
    }
  }

private:
  std::shared_ptr<AspectPriority> defaults_;
  std::shared_ptr<BuiltinOrderSource> builtin_;
};

} // namespace

std::shared_ptr<AspectPriority> make_memory_aspect_priority() {
  return std::make_shared<AspectPriorityImpl>();
}

std::shared_ptr<EntityLinkResolver> make_entity_link_resolver(
    std::shared_ptr<AspectPriority> defaults,
    std::shared_ptr<BuiltinOrderSource> builtin) {
  return std::make_shared<EntityLinkResolverImpl>(std::move(defaults),
                                                  std::move(builtin));
}

} // namespace media_library
