#include "db.h"

#include "config.h"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace db {
namespace {

std::mutex g_init_mutex;

// 后端无关：当前线程的主库会话。
thread_local std::unique_ptr<Session> t_session;

std::optional<config::DatabaseConfig> g_main_options;

// libpq 关键字连接串里的值：单引号包裹，转义反斜杠与单引号。
std::string quoteConnValue(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('\'');
  for (const char c : value) {
    if (c == '\\' || c == '\'') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

// URI 查询参数里的值需要百分号编码，避免 @ : / ? & 之类破坏连接串。
std::string encodeUriComponent(const std::string &value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);
  for (const unsigned char c : value) {
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.' || c == '~';
    if (safe) {
      out.push_back(static_cast<char>(c));
      continue;
    }
    out.push_back('%');
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0x0F]);
  }
  return out;
}

// 密码优先用配置里的明文，其次读 password_env 指定的环境变量。
std::string resolvePassword(const config::DatabasePostgresConfig &cfg) {
  if (!cfg.password.empty()) {
    return cfg.password;
  }
  if (!cfg.password_env.empty()) {
    if (const char *password = std::getenv(cfg.password_env.c_str())) {
      return password;
    }
  }
  // 都为空时留空，由服务端认证失败报错；不要在这里抛异常。
  return {};
}

SociOptions makePgOptions(const config::DatabasePostgresConfig &cfg) {
  const std::string password = resolvePassword(cfg);
  std::string conninfo = cfg.url;

  if (conninfo.find("://") != std::string::npos) {
    // URI 形式：dbname / password / sslmode / connect_timeout 作为查询参数追加。
    const auto appendQuery = [&conninfo](const std::string &key,
                                         const std::string &value) {
      conninfo.push_back(conninfo.find('?') == std::string::npos ? '?' : '&');
      conninfo += key;
      conninfo.push_back('=');
      conninfo += encodeUriComponent(value);
    };
    if (!cfg.dbname.empty()) {
      appendQuery("dbname", cfg.dbname);
    }
    if (!cfg.user.empty()) {
      appendQuery("user", cfg.user);
    }
    if (!password.empty()) {
      appendQuery("password", password);
    }
    if (!cfg.sslmode.empty()) {
      appendQuery("sslmode", cfg.sslmode);
    }
    if (cfg.connect_timeout_seconds > 0) {
      appendQuery("connect_timeout",
                  std::to_string(cfg.connect_timeout_seconds));
    }
  } else {
    // 关键字形式：把缺的字段追加到已有连接串后面。
    const auto appendKeyword = [&conninfo](const std::string &key,
                                          const std::string &value) {
      if (!conninfo.empty()) {
        conninfo.push_back(' ');
      }
      conninfo += key;
      conninfo.push_back('=');
      conninfo += quoteConnValue(value);
    };
    if (!cfg.dbname.empty()) {
      appendKeyword("dbname", cfg.dbname);
    }
    if (!cfg.user.empty()) {
      appendKeyword("user", cfg.user);
    }
    if (!password.empty()) {
      appendKeyword("password", password);
    }
    if (!cfg.sslmode.empty()) {
      appendKeyword("sslmode", cfg.sslmode);
    }
    if (cfg.connect_timeout_seconds > 0) {
      if (!conninfo.empty()) {
        conninfo.push_back(' ');
      }
      conninfo += "connect_timeout=";
      conninfo += std::to_string(cfg.connect_timeout_seconds);
    }
  }

  return SociOptions::postgres(std::move(conninfo));
}

} // namespace

void init(const config::DatabaseConfig &options) {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  g_main_options = options;
}

Session &mainSession() {
  if (t_session) {
    return *t_session;
  }
  if (!g_main_options.has_value()) {
    throw std::logic_error("db::init() 未调用，无法确定主库后端");
  }
  // 配置了 db.PostgreSQL.url 就走远端 PostgreSQL，否则用本地 SQLite。
  if (g_main_options->usePostgres()) {
    t_session =
        std::make_unique<SociSession>(makePgOptions(g_main_options->postgres));
  } else {
    t_session = std::make_unique<SociSession>(SociOptions::sqlite(
        g_main_options->sql.path, SqliteProfile::Main));
  }
  return *t_session;
}

std::unique_ptr<Session> openSqliteSession(const std::string &path,
                                           SqliteProfile profile) {
  return std::make_unique<SociSession>(
      SociOptions::sqlite(path, profile));
}

std::unique_ptr<Session> openPostgresSession(
    const config::DatabasePostgresConfig &options) {
  if (options.url.empty()) {
    return nullptr;
  }
  return std::make_unique<SociSession>(makePgOptions(options));
}

std::string makePgConninfo(const config::DatabasePostgresConfig &options) {
  return makePgOptions(options).conninfo;
}

void close() {
  t_session.reset();
}

} // namespace db
