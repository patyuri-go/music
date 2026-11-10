#ifndef DB_SOCI_SESSION_H
#define DB_SOCI_SESSION_H

#include "db/session.h"

#include <filesystem>
#include <memory>
#include <string>

namespace soci {
class session;
} // namespace soci

namespace db {

/// SOCI 会话的后端类型。
///
/// 两个后端共用同一套 SQL 绑定/取值逻辑；差异只在于：
/// - PostgreSQL 需要把 `?` 改写成 `$1..$n`；
/// - SQLite 使用原生 `?`，并且表达式列的类型要靠运行时探测（SOCI 的
///   sqlite3 后端会把无 decltype 的列一律当成 TEXT）。
enum class SociBackend { Postgres, Sqlite };

/// SQLite 打开参数。主库需要外键约束；图片缓存只需要 WAL 与宽松同步。
enum class SqliteProfile { Main, ImageCache };

struct SociOptions {
  SociBackend backend = SociBackend::Sqlite;

  /// backend == Postgres 时使用，直接交给 libpq。
  std::string conninfo;

  /// backend == Sqlite 时使用。
  std::filesystem::path sqlite_path;
  SqliteProfile profile = SqliteProfile::Main;

  static SociOptions postgres(std::string conninfo);
  static SociOptions sqlite(std::filesystem::path path, SqliteProfile profile);
};

/// 由 SOCI 同时支撑 PostgreSQL 与 SQLite 的 Session 实现。
///
/// PostgreSQL：
/// - 占位符按出现顺序 `?` → `$1..$n`；
/// - 自增 id 用 `lastval()`。
///
/// SQLite：
/// - 原生 `?`，不改写；
/// - 自增 id 用 `last_insert_rowid()`；
/// - `begin(Immediate)` 手动发 `BEGIN IMMEDIATE`（SOCI 只会发 BEGIN）；
/// - 表达式列按 sqlite3 运行时类型取值，避免 COUNT/MAX 等聚合结果被当成文本。
class SociSession final : public Session {
public:
  explicit SociSession(SociOptions options);
  ~SociSession() override;

  SociSession(const SociSession &) = delete;
  SociSession &operator=(const SociSession &) = delete;

  Dialect dialect() const noexcept override {
    return options_.backend == SociBackend::Postgres ? Dialect::Postgres
                                                     : Dialect::Sqlite;
  }

  ResultSet query(std::string_view sql, const ParamList &params) override;
  std::int64_t execute(std::string_view sql, const ParamList &params) override;
  void exec(std::string_view sql) override;
  std::int64_t lastInsertId() override;
  void begin(TransactionMode mode) override;
  void commit() override;
  void rollback() override;

private:
  SociOptions options_;
  std::unique_ptr<soci::session> session_;
};

} // namespace db

#endif
