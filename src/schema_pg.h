#ifndef SCHEMA_PG_H
#define SCHEMA_PG_H

#include "db/session.h"

#include <string>
#include <vector>

namespace schema {

/// PostgreSQL 分支的建表语句（全部 CREATE TABLE IF NOT EXISTS）。
/// 与 SQLite 版本逐条对应，只替换类型与表达式。
std::vector<std::string> postgresCreateStatements();

/// 在 PostgreSQL 空库上建 v10 schema 并校验版本。
/// 不提供旧版本升级路径：版本低于 kVersion 时抛错要求重建空库。
void ensurePostgresSchema(db::Session &session);

} // namespace schema

#endif
