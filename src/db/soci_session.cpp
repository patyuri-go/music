#include "db/soci_session.h"

#include <soci/postgresql/soci-postgresql.h>
#include <soci/soci.h>
#include <soci/sqlite3/soci-sqlite3.h>

#include <sqlite3.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace db {
namespace {

// PostgreSQL 的 SQLSTATE 前两位是错误类别，比 SOCI 的粗粒度 category 更精确。
// SQLite 的 extended result code 低 8 位是主码，高 8 位是细分原因。
ErrorKind mapSociError(const soci::soci_error &error) {
  if (const auto *pg_error =
          dynamic_cast<const soci::postgresql_soci_error *>(&error)) {
    const std::string state = pg_error->get_sqlstate();
    if (state.size() >= 2) {
      const std::string klass = state.substr(0, 2);
      if (state == "23502") {
        return ErrorKind::NotNull;
      }
      if (klass == "23") {
        return ErrorKind::Constraint;
      }
      if (klass == "28" || state == "42501") {
        return ErrorKind::Auth;
      }
      if (klass == "08") {
        return ErrorKind::Unavailable;
      }
      if (state == "40001" || state == "40P01" || state == "55P03" ||
          state == "57014" || state == "53300") {
        return ErrorKind::Busy;
      }
      if (state == "0A000") {
        return ErrorKind::Unsupported;
      }
    }
  }

  if (const auto *sqlite_error =
          dynamic_cast<const soci::sqlite3_soci_error *>(&error)) {
    const int code = sqlite_error->extended_result();
    switch (code & 0xff) {
    case SQLITE_CONSTRAINT:
      if (code == SQLITE_CONSTRAINT_NOTNULL) {
        return ErrorKind::NotNull;
      }
      return ErrorKind::Constraint;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
      return ErrorKind::Busy;
    case SQLITE_READONLY:
    case SQLITE_CANTOPEN:
    case SQLITE_IOERR:
      return ErrorKind::Unavailable;
    default:
      break;
    }
  }

  switch (error.get_error_category()) {
  case soci::soci_error::connection_error:
    return ErrorKind::Unavailable;
  case soci::soci_error::constraint_violation:
    return ErrorKind::Constraint;
  case soci::soci_error::no_privilege:
    return ErrorKind::Auth;
  case soci::soci_error::unknown_transaction_state:
  case soci::soci_error::system_error:
    return ErrorKind::Busy;
  default:
    return ErrorKind::Internal;
  }
}

// PostgreSQL 后端不认识 `?`，只支持 `:name`（SOCI 再改写成 `$n`）。
// 这里直接把 `?` 改写成 `$1..$n`，让上层 SQL 保持 SQLite 风格。
// 跳过单引号字符串、双引号标识符、`--` 行注释与 `/* */` 块注释。
std::string rewritePlaceholders(std::string_view sql) {
  std::string out;
  out.reserve(sql.size() + 16);
  int index = 0;

  for (std::size_t i = 0; i < sql.size(); ++i) {
    const char c = sql[i];

    if (c == '\'') {
      out.push_back(c);
      ++i;
      while (i < sql.size()) {
        if (sql[i] == '\'' && i + 1 < sql.size() && sql[i + 1] == '\'') {
          out.push_back(sql[i]);
          out.push_back(sql[i + 1]);
          i += 2;
          continue;
        }
        out.push_back(sql[i]);
        if (sql[i] == '\'') {
          break;
        }
        ++i;
      }
      continue;
    }

    if (c == '"') {
      out.push_back(c);
      ++i;
      while (i < sql.size()) {
        out.push_back(sql[i]);
        if (sql[i] == '"') {
          break;
        }
        ++i;
      }
      continue;
    }

    if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
      while (i < sql.size() && sql[i] != '\n') {
        out.push_back(sql[i]);
        ++i;
      }
      if (i < sql.size()) {
        out.push_back(sql[i]);
      }
      continue;
    }

    if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
      out.push_back('/');
      out.push_back('*');
      i += 2;
      while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/')) {
        out.push_back(sql[i]);
        ++i;
      }
      if (i + 1 < sql.size()) {
        out.push_back('*');
        out.push_back('/');
        ++i;
      }
      continue;
    }

    if (c == '?') {
      out.push_back('$');
      out += std::to_string(++index);
      continue;
    }

    out.push_back(c);
  }

  return out;
}

// 参数槽位：soci::use 只接受左值引用，因此参数值必须存进寿命覆盖
// prepare/execute 的容器里。
struct Slot {
  enum class Kind { Null, Integer, Real, Text };
  Kind kind{Kind::Null};
  std::int64_t integer{0};
  double real{0.0};
  std::string text;
  soci::indicator indicator{soci::i_ok};
};

std::vector<Slot> makeSlots(const ParamList &params) {
  std::vector<Slot> slots;
  slots.reserve(params.size());
  for (const Value &value : params) {
    Slot slot;
    if (std::holds_alternative<Null>(value)) {
      slot.kind = Slot::Kind::Null;
      slot.indicator = soci::i_null;
    } else if (const auto *number = std::get_if<std::int64_t>(&value)) {
      slot.kind = Slot::Kind::Integer;
      slot.integer = *number;
    } else if (const auto *real = std::get_if<double>(&value)) {
      slot.kind = Slot::Kind::Real;
      slot.real = *real;
    } else {
      slot.kind = Slot::Kind::Text;
      slot.text = std::get<std::string>(value);
    }
    slots.push_back(std::move(slot));
  }
  return slots;
}

void bindSlots(soci::statement &statement, std::vector<Slot> &slots) {
  for (Slot &slot : slots) {
    switch (slot.kind) {
    case Slot::Kind::Integer:
      statement.exchange(soci::use(slot.integer));
      break;
    case Slot::Kind::Real:
      statement.exchange(soci::use(slot.real));
      break;
    case Slot::Kind::Text:
      statement.exchange(soci::use(slot.text, slot.indicator));
      break;
    case Slot::Kind::Null:
    default:
      // NULL 借字符串槽位带 i_null 传出。
      statement.exchange(soci::use(slot.text, slot.indicator));
      break;
    }
  }
}

std::string formatTm(const std::tm &value) {
  char buffer[32] = {0};
  const std::size_t written =
      std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &value);
  if (written == 0) {
    return {};
  }
  return std::string(buffer, written);
}

bool parseInt64(const std::string &text, std::int64_t &out) {
  if (text.empty()) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const long long value = std::strtoll(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0') {
    return false;
  }
  out = static_cast<std::int64_t>(value);
  return true;
}

bool parseDouble(const std::string &text, double &out) {
  if (text.empty()) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (errno != 0 || end == text.c_str() || *end != '\0') {
    return false;
  }
  out = value;
  return true;
}

Cell readPostgresCell(const soci::row &row, std::size_t index) {
  if (row.get_indicator(index) == soci::i_null) {
    return Cell{Value{Null{}}};
  }

  switch (row.get_properties(index).get_db_type()) {
  case soci::db_string:
    return Cell{Value{row.get<std::string>(index)}};
  case soci::db_double:
    return Cell{Value{row.get<double>(index)}};
  case soci::db_int8:
    return Cell{Value{static_cast<std::int64_t>(row.get<std::int8_t>(index))}};
  case soci::db_int16:
    return Cell{Value{static_cast<std::int64_t>(row.get<std::int16_t>(index))}};
  case soci::db_int32:
    return Cell{Value{static_cast<std::int64_t>(row.get<std::int32_t>(index))}};
  case soci::db_int64:
    return Cell{Value{static_cast<std::int64_t>(row.get<std::int64_t>(index))}};
  case soci::db_date:
    return Cell{Value{formatTm(row.get<std::tm>(index))}};
  case soci::db_blob:
  default:
    // Schema 中没有 BLOB 列；未知类型按 NULL 兜底，避免静默编造值。
    return Cell{Value{Null{}}};
  }
}

// SOCI 的 sqlite3 后端把没有 decltype 的表达式列（COUNT/MAX/SUM/…）一律
// 描述成 db_string，于是数值结果会以文本形式出现在 soci::row 里。
// 这里用 sqlite3_column_type() 读取当前行的真实类型：
// - 表达式返回整数/浮点 → 还原成 int64/double；
// - 普通 TEXT 列 → 保持 string。
Cell readSqliteCell(const soci::row &row, std::size_t index,
                    sqlite3_stmt *stmt) {
  if (row.get_indicator(index) == soci::i_null) {
    return Cell{Value{Null{}}};
  }

  const soci::db_type declared = row.get_properties(index).get_db_type();
  if (declared == soci::db_string) {
    const std::string text = row.get<std::string>(index);
    const int runtime =
        stmt != nullptr ? sqlite3_column_type(stmt, static_cast<int>(index))
                        : SQLITE_TEXT;
    if (runtime == SQLITE_INTEGER) {
      std::int64_t number = 0;
      if (parseInt64(text, number)) {
        return Cell{Value{number}};
      }
    } else if (runtime == SQLITE_FLOAT) {
      double number = 0.0;
      if (parseDouble(text, number)) {
        return Cell{Value{number}};
      }
    }
    return Cell{Value{text}};
  }

  switch (declared) {
  case soci::db_double:
    return Cell{Value{row.get<double>(index)}};
  case soci::db_int8:
    return Cell{Value{static_cast<std::int64_t>(row.get<std::int8_t>(index))}};
  case soci::db_int16:
    return Cell{Value{static_cast<std::int64_t>(row.get<std::int16_t>(index))}};
  case soci::db_int32:
    return Cell{Value{static_cast<std::int64_t>(row.get<std::int32_t>(index))}};
  case soci::db_int64:
    return Cell{Value{static_cast<std::int64_t>(row.get<std::int64_t>(index))}};
  case soci::db_date:
    return Cell{Value{formatTm(row.get<std::tm>(index))}};
  case soci::db_blob:
  default:
    return Cell{Value{Null{}}};
  }
}

// SQLite 连接串：路径中若含 '=' 或空格，SOCI 会按 key=value 解析，
// 因此改用引号包裹的 dbname 形式。
std::string sqliteConnectString(const std::filesystem::path &path) {
  const std::string name = path.string();
  if (name.find('=') == std::string::npos &&
      name.find(' ') == std::string::npos) {
    return name;
  }
  return "dbname='" + name + "'";
}

} // namespace

SociOptions SociOptions::postgres(std::string conninfo) {
  SociOptions options;
  options.backend = SociBackend::Postgres;
  options.conninfo = std::move(conninfo);
  return options;
}

SociOptions SociOptions::sqlite(std::filesystem::path path,
                               SqliteProfile profile) {
  SociOptions options;
  options.backend = SociBackend::Sqlite;
  options.sqlite_path = std::move(path);
  options.profile = profile;
  return options;
}

SociSession::SociSession(SociOptions options)
    : options_(std::move(options)) {
  try {
    if (options_.backend == SociBackend::Postgres) {
      session_ = std::make_unique<soci::session>(soci::postgresql,
                                                 options_.conninfo);
    } else {
      session_ = std::make_unique<soci::session>(
          soci::sqlite3, sqliteConnectString(options_.sqlite_path));
      // SQLite 默认 busy_timeout 是 0；WAL 与 synchronous 与旧实现保持一致。
      if (options_.profile == SqliteProfile::Main) {
        *session_ << "PRAGMA foreign_keys=ON";
        *session_ << "PRAGMA journal_mode=WAL";
        *session_ << "PRAGMA busy_timeout=5000";
      } else {
        *session_ << "PRAGMA journal_mode=WAL";
        *session_ << "PRAGMA synchronous=NORMAL";
        *session_ << "PRAGMA busy_timeout=5000";
      }
    }

    // SOCI 默认的 log_context::always 会让 statement_impl::pre_use() 调
    // do_add_query_parameters()，进而向后端索取每个 use 元素的参数名。
    // 本 adapter 自己改写 `?`，后端不会填参数名表，at() 会越界。
    // 关掉查询上下文记录即可绕开这条只用于日志的路径。
    session_->set_query_context_logging_mode(soci::log_context::never);
  } catch (const soci::soci_error &error) {
    throw Error(mapSociError(error), error.what());
  }
}

SociSession::~SociSession() = default;

ResultSet SociSession::query(std::string_view sql, const ParamList &params) {
  try {
    std::vector<Slot> slots = makeSlots(params);

    const std::string prepared_sql =
        options_.backend == SociBackend::Postgres ? rewritePlaceholders(sql)
                                                  : std::string(sql);

    soci::statement statement(*session_);
    statement.alloc();
    statement.prepare(prepared_sql, soci::details::st_one_time_query);

    bindSlots(statement, slots);

    // 这里不能用 exchange_for_rowset()：它内部会立刻对 row 调 define()，
    // 随后 define_and_bind() 会重复 define 而抛 "Only one Row element allowed"。
    soci::row row;
    soci::details::into_type_ptr row_into(
        new soci::details::into_type<soci::row>(row));
    statement.exchange(row_into);
    statement.define_and_bind();
    statement.execute(false);

    sqlite3_stmt *sqlite_stmt = nullptr;
    if (options_.backend == SociBackend::Sqlite) {
      auto *backend = dynamic_cast<soci::sqlite3_statement_backend *>(
          statement.get_backend());
      if (backend != nullptr) {
        sqlite_stmt = reinterpret_cast<sqlite3_stmt *>(backend->stmt_);
      }
    }

    ResultSet result;
    bool described = false;
    while (statement.fetch()) {
      if (!described) {
        result.columns.reserve(row.size());
        for (std::size_t i = 0; i < row.size(); ++i) {
          result.columns.push_back(row.get_properties(i).get_name());
        }
        described = true;
      }
      Row values;
      values.reserve(row.size());
      for (std::size_t i = 0; i < row.size(); ++i) {
        values.push_back(options_.backend == SociBackend::Sqlite
                             ? readSqliteCell(row, i, sqlite_stmt)
                             : readPostgresCell(row, i));
      }
      result.rows.push_back(std::move(values));
    }

    // 0 行时也要给出列名，否则调用方无法区分"没数据"和"没这一列"。
    if (!described) {
      result.columns.reserve(row.size());
      for (std::size_t i = 0; i < row.size(); ++i) {
        result.columns.push_back(row.get_properties(i).get_name());
      }
    }
    return result;
  } catch (const soci::soci_error &error) {
    throw Error(mapSociError(error), error.what());
  }
}

std::int64_t SociSession::execute(std::string_view sql,
                                  const ParamList &params) {
  try {
    std::vector<Slot> slots = makeSlots(params);

    const std::string prepared_sql =
        options_.backend == SociBackend::Postgres ? rewritePlaceholders(sql)
                                                  : std::string(sql);

    soci::statement statement(*session_);
    statement.alloc();
    statement.prepare(prepared_sql, soci::details::st_one_time_query);
    bindSlots(statement, slots);
    statement.define_and_bind();
    // SQLite 后端在 execute(false) 时不会真正 step（fetch(0) 直接返回），
    // 必须传 true 才会执行写语句。
    statement.execute(options_.backend == SociBackend::Sqlite);
    return static_cast<std::int64_t>(statement.get_affected_rows());
  } catch (const soci::soci_error &error) {
    throw Error(mapSociError(error), error.what());
  }
}

void SociSession::exec(std::string_view sql) {
  try {
    *session_ << std::string(sql);
  } catch (const soci::soci_error &error) {
    throw Error(mapSociError(error), error.what());
  }
}

std::int64_t SociSession::lastInsertId() {
  try {
    const ResultSet result =
        query(options_.backend == SociBackend::Sqlite
                  ? "SELECT last_insert_rowid()"
                  : "SELECT lastval()",
              {});
    if (result.rows.empty() || result.rows.front().empty()) {
      throw Error(ErrorKind::Unsupported,
                  "SociSession::lastInsertId: 会话内没有可用的自增 id");
    }
    const Value &value = result.rows.front().front().value;
    if (const auto *number = std::get_if<std::int64_t>(&value)) {
      return *number;
    }
    throw Error(ErrorKind::Internal,
                "SociSession::lastInsertId: 查询结果非整数");
  } catch (const soci::soci_error &error) {
    throw Error(mapSociError(error), error.what());
  }
}

void SociSession::begin(TransactionMode mode) {
  try {
    if (options_.backend == SociBackend::Sqlite &&
        mode == TransactionMode::Immediate) {
      exec("BEGIN IMMEDIATE");
    } else {
      // PostgreSQL 没有 BEGIN IMMEDIATE；写冲突由行锁与隔离级别处理。
      session_->begin();
    }
  } catch (const soci::soci_error &error) {
    throw Error(mapSociError(error), error.what());
  }
}

void SociSession::commit() {
  try {
    session_->commit();
  } catch (const soci::soci_error &error) {
    throw Error(mapSociError(error), error.what());
  }
}

void SociSession::rollback() {
  try {
    session_->rollback();
  } catch (const soci::soci_error &error) {
    throw Error(mapSociError(error), error.what());
  }
}

} // namespace db
