#include "media_library/entity_link.h"

#include "db.h"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>
#include <SQLiteCpp/Transaction.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <exception>
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

Error sqlite_error(const SQLite::Exception &error, const char *context) {
  if (error.getErrorCode() == SQLITE_CONSTRAINT) {
    return Error::make(ErrorCode::Conflict,
                       std::string(context) + ": " + error.what());
  }
  return Error::make(ErrorCode::Internal,
                     std::string(context) + ": " + error.what());
}

Error internal_error(const std::exception &error, const char *context) {
  return Error::make(ErrorCode::Internal,
                     std::string(context) + ": " + error.what());
}

std::optional<LocatedLink> locate_link(SQLite::Database &database,
                                       const SourceRef &ref) {
  SQLite::Statement statement(
      database,
      "SELECT l.link_id, l.entity_type "
      "FROM entity_link_member m "
      "JOIN entity_link l ON l.link_id = m.link_id "
      "WHERE m.library_id = ? AND m.entity_type = ? AND m.entity_id = ?");
  statement.bind(1, ref.library_id);
  statement.bind(2, entity_type_text(ref.entity_type));
  statement.bind(3, ref.entity_id);
  if (!statement.executeStep()) {
    return std::nullopt;
  }
  const std::optional<EntityType> type =
      parse_entity_type(statement.getColumn(1).getString());
  if (!type.has_value()) {
    return std::nullopt;
  }
  LocatedLink located;
  located.link_id = statement.getColumn(0).getInt64();
  located.entity_type = *type;
  return located;
}

std::vector<std::string> member_libraries(SQLite::Database &database,
                                          std::int64_t link_id) {
  SQLite::Statement statement(
      database,
      "SELECT library_id FROM entity_link_member "
      "WHERE link_id = ? ORDER BY member_id");
  statement.bind(1, link_id);
  std::vector<std::string> libraries;
  while (statement.executeStep()) {
    libraries.push_back(statement.getColumn(0).getString());
  }
  return libraries;
}

LinkOverrides load_overrides(SQLite::Database &database,
                             std::int64_t link_id) {
  SQLite::Statement statement(
      database,
      "SELECT aspect, library_id FROM entity_link_override "
      "WHERE link_id = ? ORDER BY aspect, position");
  statement.bind(1, link_id);
  LinkOverrides overrides;
  while (statement.executeStep()) {
    const std::optional<Aspect> aspect =
        parse_aspect(statement.getColumn(0).getString());
    if (!aspect.has_value()) {
      continue;
    }
    overrides.order[*aspect].push_back(statement.getColumn(1).getString());
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

void insert_overrides(SQLite::Database &database, std::int64_t link_id,
                      const LinkOverrides &overrides) {
  for (const auto &entry : overrides.order) {
    SQLite::Statement statement(
        database,
        "INSERT INTO entity_link_override"
        "(link_id, aspect, library_id, position) VALUES(?, ?, ?, ?)");
    int position = 0;
    for (const std::string &library_id : entry.second) {
      statement.bind(1, link_id);
      statement.bind(2, aspect_text(entry.first));
      statement.bind(3, library_id);
      statement.bind(4, position);
      statement.exec();
      statement.reset();
      ++position;
    }
  }
}

void touch_link(SQLite::Database &database, std::int64_t link_id) {
  SQLite::Statement statement(
      database,
      "UPDATE entity_link SET updated_at = datetime('now','localtime') "
      "WHERE link_id = ?");
  statement.bind(1, link_id);
  statement.exec();
}

std::int64_t insert_link_row(SQLite::Database &database,
                             EntityType entity_type) {
  SQLite::Statement statement(database,
                              "INSERT INTO entity_link(entity_type) VALUES(?)");
  statement.bind(1, entity_type_text(entity_type));
  statement.exec();
  return database.getLastInsertRowid();
}

void insert_member(SQLite::Database &database, std::int64_t link_id,
                   const LinkedRef &item) {
  SQLite::Statement statement(
      database,
      "INSERT INTO entity_link_member"
      "(link_id, library_id, entity_type, entity_id, confidence, verified) "
      "VALUES(?, ?, ?, ?, ?, ?)");
  statement.bind(1, link_id);
  statement.bind(2, item.ref.library_id);
  statement.bind(3, entity_type_text(item.ref.entity_type));
  statement.bind(4, item.ref.entity_id);
  statement.bind(5, item.confidence.empty() ? "manual" : item.confidence);
  statement.bind(6, item.verified ? 1 : 0);
  statement.exec();
}

bool link_has_library(SQLite::Database &database, std::int64_t link_id,
                      const std::string &library_id) {
  SQLite::Statement statement(
      database,
      "SELECT 1 FROM entity_link_member WHERE link_id = ? AND library_id = ?");
  statement.bind(1, link_id);
  statement.bind(2, library_id);
  return statement.executeStep();
}

class EntityLinkResolverImpl final : public EntityLinkResolver {
public:
  EntityLinkResolverImpl(std::string db_path,
                         std::shared_ptr<AspectPriority> defaults,
                         std::shared_ptr<BuiltinOrderSource> builtin)
      : db_path_(std::move(db_path)), defaults_(std::move(defaults)),
        builtin_(std::move(builtin)) {}

  Result<EntityLinkView> find_link(const SourceRef &ref,
                                   const CallContext &) const override {
    try {
      SQLite::Database &database = db::conn(db_path_);
      SQLite::Transaction transaction(database,
                                      SQLite::TransactionBehavior::DEFERRED);
      const std::optional<LocatedLink> located = locate_link(database, ref);
      if (!located.has_value()) {
        return Result<EntityLinkView>::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }

      EntityLinkView view;
      view.entity_type = located->entity_type;

      SQLite::Statement head(
          database,
          "SELECT created_at, updated_at FROM entity_link WHERE link_id = ?");
      head.bind(1, located->link_id);
      if (!head.executeStep()) {
        return Result<EntityLinkView>::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }
      view.created_at = head.getColumn(0).getString();
      if (!head.getColumn(1).isNull()) {
        view.updated_at = head.getColumn(1).getString();
      }

      SQLite::Statement members(
          database,
          "SELECT library_id, entity_type, entity_id, confidence, verified "
          "FROM entity_link_member WHERE link_id = ? ORDER BY member_id");
      members.bind(1, located->link_id);
      while (members.executeStep()) {
        const std::optional<EntityType> type =
            parse_entity_type(members.getColumn(1).getString());
        if (!type.has_value()) {
          return Result<EntityLinkView>::failure(Error::make(
              ErrorCode::Internal, "invalid entity_type in entity_link_member"));
        }
        LinkedRef item;
        item.ref.library_id = members.getColumn(0).getString();
        item.ref.entity_type = *type;
        item.ref.entity_id = members.getColumn(2).getString();
        item.confidence = members.getColumn(3).getString();
        item.verified = members.getColumn(4).getInt() != 0;
        view.refs.push_back(std::move(item));
      }

      view.overrides = load_overrides(database, located->link_id);
      transaction.commit();
      return Result<EntityLinkView>::success(std::move(view));
    } catch (const SQLite::Exception &error) {
      return Result<EntityLinkView>::failure(
          sqlite_error(error, "find_link failed"));
    } catch (const std::exception &error) {
      return Result<EntityLinkView>::failure(
          internal_error(error, "find_link failed"));
    }
  }

  Result<std::vector<LinkedRef>>
  refs_of(const SourceRef &ref, const CallContext &) const override {
    try {
      SQLite::Database &database = db::conn(db_path_);
      const std::optional<LocatedLink> located = locate_link(database, ref);
      if (!located.has_value()) {
        return Result<std::vector<LinkedRef>>::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }

      SQLite::Statement statement(
          database,
          "SELECT library_id, entity_type, entity_id, confidence, verified "
          "FROM entity_link_member WHERE link_id = ? ORDER BY member_id");
      statement.bind(1, located->link_id);
      std::vector<LinkedRef> refs;
      while (statement.executeStep()) {
        const std::optional<EntityType> type =
            parse_entity_type(statement.getColumn(1).getString());
        if (!type.has_value()) {
          return Result<std::vector<LinkedRef>>::failure(Error::make(
              ErrorCode::Internal, "invalid entity_type in entity_link_member"));
        }
        LinkedRef item;
        item.ref.library_id = statement.getColumn(0).getString();
        item.ref.entity_type = *type;
        item.ref.entity_id = statement.getColumn(2).getString();
        item.confidence = statement.getColumn(3).getString();
        item.verified = statement.getColumn(4).getInt() != 0;
        refs.push_back(std::move(item));
      }
      return Result<std::vector<LinkedRef>>::success(std::move(refs));
    } catch (const SQLite::Exception &error) {
      return Result<std::vector<LinkedRef>>::failure(
          sqlite_error(error, "refs_of failed"));
    } catch (const std::exception &error) {
      return Result<std::vector<LinkedRef>>::failure(
          internal_error(error, "refs_of failed"));
    }
  }

  Result<LinkOverrides>
  overrides_of(const SourceRef &ref, const CallContext &) const override {
    try {
      SQLite::Database &database = db::conn(db_path_);
      const std::optional<LocatedLink> located = locate_link(database, ref);
      if (!located.has_value()) {
        return Result<LinkOverrides>::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }
      return Result<LinkOverrides>::success(
          load_overrides(database, located->link_id));
    } catch (const SQLite::Exception &error) {
      return Result<LinkOverrides>::failure(
          sqlite_error(error, "overrides_of failed"));
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
      SQLite::Database &database = db::conn(db_path_);
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

      SQLite::Statement statement(database, sql);
      int index = 1;
      if (type.has_value()) {
        statement.bind(index++, entity_type_text(*type));
      }
      statement.bind(index++, limit);
      statement.bind(index++, offset);

      std::vector<EntityLinkSummary> summaries;
      while (statement.executeStep()) {
        const std::optional<EntityType> link_type =
            parse_entity_type(statement.getColumn(1).getString());
        const std::optional<EntityType> anchor_type =
            parse_entity_type(statement.getColumn(6).getString());
        if (!link_type.has_value() || !anchor_type.has_value()) {
          return Result<std::vector<EntityLinkSummary>>::failure(
              Error::make(ErrorCode::Internal, "invalid entity_type in link"));
        }
        EntityLinkSummary summary;
        summary.anchor.library_id = statement.getColumn(5).getString();
        summary.anchor.entity_type = *anchor_type;
        summary.anchor.entity_id = statement.getColumn(7).getString();
        summary.entity_type = *link_type;
        summary.member_count = statement.getColumn(4).getInt();
        summary.created_at = statement.getColumn(2).getString();
        if (!statement.getColumn(3).isNull()) {
          summary.updated_at = statement.getColumn(3).getString();
        }
        summaries.push_back(std::move(summary));
      }
      return Result<std::vector<EntityLinkSummary>>::success(
          std::move(summaries));
    } catch (const SQLite::Exception &error) {
      return Result<std::vector<EntityLinkSummary>>::failure(
          sqlite_error(error, "list_links failed"));
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
      SQLite::Database &database = db::conn(db_path_);
      SQLite::Transaction transaction(database,
                                      SQLite::TransactionBehavior::IMMEDIATE);

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
    } catch (const SQLite::Exception &error) {
      return Status::failure(sqlite_error(error, "link failed"));
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
      SQLite::Database &database = db::conn(db_path_);
      SQLite::Transaction transaction(database,
                                      SQLite::TransactionBehavior::IMMEDIATE);
      const std::optional<LocatedLink> located = locate_link(database, ref);
      if (!located.has_value()) {
        return Status::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }

      SQLite::Statement remove(
          database,
          "DELETE FROM entity_link_member "
          "WHERE library_id = ? AND entity_type = ? AND entity_id = ?");
      remove.bind(1, ref.library_id);
      remove.bind(2, entity_type_text(ref.entity_type));
      remove.bind(3, ref.entity_id);
      remove.exec();
      if (database.getChanges() == 0) {
        return Status::failure(
            Error::make(ErrorCode::NotFound, "entity link member not found"));
      }

      SQLite::Statement count(
          database,
          "SELECT COUNT(*) FROM entity_link_member WHERE link_id = ?");
      count.bind(1, located->link_id);
      if (!count.executeStep()) {
        return Status::failure(
            Error::make(ErrorCode::Internal, "failed to count link members"));
      }
      const int remaining = count.getColumn(0).getInt();

      if (remaining < 2) {
        SQLite::Statement remove_link(
            database, "DELETE FROM entity_link WHERE link_id = ?");
        remove_link.bind(1, located->link_id);
        remove_link.exec();
      } else {
        touch_link(database, located->link_id);
      }

      transaction.commit();
      return Status::success();
    } catch (const SQLite::Exception &error) {
      return Status::failure(sqlite_error(error, "remove_ref failed"));
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
      SQLite::Database &database = db::conn(db_path_);
      SQLite::Transaction transaction(database,
                                      SQLite::TransactionBehavior::IMMEDIATE);
      const std::optional<LocatedLink> located = locate_link(database, anchor);
      if (!located.has_value()) {
        return Status::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }
      SQLite::Statement remove_link(
          database, "DELETE FROM entity_link WHERE link_id = ?");
      remove_link.bind(1, located->link_id);
      remove_link.exec();
      transaction.commit();
      return Status::success();
    } catch (const SQLite::Exception &error) {
      return Status::failure(sqlite_error(error, "delete_link failed"));
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
      SQLite::Database &database = db::conn(db_path_);
      SQLite::Transaction transaction(database,
                                      SQLite::TransactionBehavior::IMMEDIATE);
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

      SQLite::Statement clear(
          database, "DELETE FROM entity_link_override WHERE link_id = ?");
      clear.bind(1, located->link_id);
      clear.exec();

      insert_overrides(database, located->link_id, overrides);
      touch_link(database, located->link_id);
      transaction.commit();
      return Status::success();
    } catch (const SQLite::Exception &error) {
      return Status::failure(sqlite_error(error, "set_overrides failed"));
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
      SQLite::Database &database = db::conn(db_path_);
      SQLite::Transaction transaction(database,
                                      SQLite::TransactionBehavior::IMMEDIATE);
      const std::optional<LocatedLink> located = locate_link(database, anchor);
      if (!located.has_value()) {
        return Status::failure(
            Error::make(ErrorCode::NotFound, "entity link not found"));
      }

      SQLite::Statement clear(
          database,
          "DELETE FROM entity_link_override WHERE link_id = ? AND aspect = ?");
      clear.bind(1, located->link_id);
      clear.bind(2, aspect_text(aspect));
      clear.exec();
      touch_link(database, located->link_id);
      transaction.commit();
      return Status::success();
    } catch (const SQLite::Exception &error) {
      return Status::failure(sqlite_error(error, "clear_overrides failed"));
    } catch (const std::exception &error) {
      return Status::failure(internal_error(error, "clear_overrides failed"));
    }
  }

  std::vector<std::string> effective_order(const SourceRef &anchor,
                                           Aspect aspect) const override {
    try {
      SQLite::Database &database = db::conn(db_path_);
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
      SQLite::Statement statement(
          database,
          "SELECT library_id FROM entity_link_override "
          "WHERE link_id = ? AND aspect = ? ORDER BY position");
      statement.bind(1, located->link_id);
      statement.bind(2, aspect_text(aspect));
      while (statement.executeStep()) {
        preferred.push_back(statement.getColumn(0).getString());
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
      std::vector<std::string> result;
      result.reserve(available.size());
      for (const std::string &library_id : preferred) {
        if (available_set.count(library_id) == 0) {
          continue;
        }
        if (seen.insert(library_id).second) {
          result.push_back(library_id);
        }
      }
      for (const std::string &library_id : available) {
        if (seen.insert(library_id).second) {
          result.push_back(library_id);
        }
      }
      return result;
    } catch (const SQLite::Exception &) {
      return {};
    } catch (const std::exception &) {
      return {};
    }
  }

private:
  std::string db_path_;
  std::shared_ptr<AspectPriority> defaults_;
  std::shared_ptr<BuiltinOrderSource> builtin_;
};

} // namespace

std::shared_ptr<AspectPriority> make_memory_aspect_priority() {
  return std::make_shared<AspectPriorityImpl>();
}

std::shared_ptr<EntityLinkResolver> make_sqlite_entity_link_resolver(
    std::string db_path, std::shared_ptr<AspectPriority> defaults,
    std::shared_ptr<BuiltinOrderSource> builtin) {
  return std::make_shared<EntityLinkResolverImpl>(
      std::move(db_path), std::move(defaults), std::move(builtin));
}

} // namespace media_library
