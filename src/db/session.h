#ifndef DB_SESSION_H
#define DB_SESSION_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace db {

/// SQL 方言。后端决定，不由调用点猜测。
enum class Dialect {
  Sqlite,
  Postgres,
};

/// 事务模式的语义提示：
/// - Deferred：默认行为；
/// - Immediate：SQLite 下立即取写锁；HTTP 后端无法表达，由服务端决定。
enum class TransactionMode {
  Deferred,
  Immediate,
};

/// SQL NULL。用独立类型而不是 std::optional，便于 JSON 序列化与 variant 分派。
struct Null {};

using Value = std::variant<Null, std::int64_t, double, std::string>;
using ParamList = std::vector<Value>;

struct Cell {
  Value value;

  bool is_null() const { return std::holds_alternative<Null>(value); }
};

using Row = std::vector<Cell>;

struct ResultSet {
  std::vector<std::string> columns;
  std::vector<Row> rows;

  bool empty() const { return rows.empty(); }
  std::size_t size() const { return rows.size(); }
};

/// 数据库错误类别。上层只需要区分这几类，不依赖具体后端。
enum class ErrorKind {
  Constraint,
  NotNull,
  Busy,
  Timeout,
  Unavailable,
  Auth,
  Unsupported,
  Internal,
};

const char *errorKindName(ErrorKind kind);

class Error : public std::runtime_error {
public:
  Error(ErrorKind kind, const std::string &message)
      : std::runtime_error(message), kind_(kind) {}

  ErrorKind kind() const noexcept { return kind_; }

private:
  ErrorKind kind_;
};

/// 所有 SQL 访问的统一入口。
///
/// 当前实现：SOCI（PostgreSQL 或本地 SQLite）与远端 HTTP SQL 服务。
/// Session 非线程安全：每个线程各自持有，不跨线程共享。
class Session {
public:
  virtual ~Session() = default;

  virtual Dialect dialect() const noexcept = 0;

  /// 执行查询并物化全部行。没有结果时返回空 rows，不抛异常。
  virtual ResultSet query(std::string_view sql,
                          const ParamList &params = {}) = 0;

  /// 执行写入，返回受影响行数（原 getChanges()）。
  virtual std::int64_t execute(std::string_view sql,
                               const ParamList &params = {}) = 0;

  /// 执行无参语句：DDL、PRAGMA、BEGIN/COMMIT 等。
  virtual void exec(std::string_view sql) = 0;

  /// 紧接上一条 INSERT 之后取新行 id（原 getLastInsertRowid()）。
  /// 后端不支持时抛 Error(ErrorKind::Unsupported)。
  virtual std::int64_t lastInsertId() = 0;

  virtual void begin(TransactionMode mode) = 0;
  virtual void commit() = 0;
  virtual void rollback() = 0;
};

/// RAII 事务：析构时未 commit 则 rollback。
class Transaction {
public:
  explicit Transaction(Session &session,
                       TransactionMode mode = TransactionMode::Deferred);
  ~Transaction();

  void commit();
  void rollback();

  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;

private:
  Session &session_;
  bool done_ = false;
};

} // namespace db

#endif
