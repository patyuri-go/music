#ifndef DB_H
#define DB_H

#include "db/session.h"
#include "db/soci_session.h"

#include <memory>
#include <string>

namespace config {
struct DatabaseConfig;
struct DatabasePostgresConfig;
} // namespace config

namespace db {

// ---- 后端无关接口 ----
/// 用配置确定主库后端。进程启动时调用一次；不发起阻塞式探测。
void init(const config::DatabaseConfig &options);

/// 当前线程的主库会话。每线程一个，不跨线程共享。
Session &mainSession();

/// 打开一个独立的本地 SQLite 会话（不参与主库后端选择），
/// 例如 image_cache 的 index.db。
std::unique_ptr<Session> openSqliteSession(const std::string &path,
                                           SqliteProfile profile);

/// 打开一个独立的 PostgreSQL 会话（不参与主库后端选择），
/// 例如云端实体库 entity。`url` 为空时返回 nullptr。
std::unique_ptr<Session> openPostgresSession(
    const config::DatabasePostgresConfig &options);

/// 由 db.PostgreSQL 配置拼出 libpq 连接串（含密码）。
/// **不要写进日志**。
std::string makePgConninfo(const config::DatabasePostgresConfig &options);

/// 释放当前线程持有的会话。
void close();

} // namespace db

#endif
