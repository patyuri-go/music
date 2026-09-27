#ifndef DB_H
#define DB_H

#include <mutex>
#include <string>

namespace SQLite {
class Database;
} // namespace SQLite

namespace db {

extern std::mutex g_db_mutex;

SQLite::Database &conn(const std::string &path);
void applyPragmas(SQLite::Database &database);
void close();

} // namespace db

#endif
