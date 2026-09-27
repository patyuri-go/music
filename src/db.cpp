#include "db.h"

#include "SQLiteCpp/Database.h"

#include <memory>
#include <mutex>

namespace db {
namespace {

thread_local std::unique_ptr<SQLite::Database> t_connection;
std::mutex g_init_mutex;

} // namespace

void applyPragmas(SQLite::Database &database) {
  database.exec("PRAGMA foreign_keys = ON;");
  database.exec("PRAGMA journal_mode = WAL;");
  database.exec("PRAGMA busy_timeout = 5000;");
}

SQLite::Database &conn(const std::string &path) {
  if (!t_connection) {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (!t_connection) {
      t_connection = std::make_unique<SQLite::Database>(
          path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
      applyPragmas(*t_connection);
    }
  }
  return *t_connection;
}

void close() { t_connection.reset(); }

} // namespace db
