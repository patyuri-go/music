#ifndef SUBSONIC_ACCESS_LOG_H
#define SUBSONIC_ACCESS_LOG_H

#include "crow/http_request.h"

#include <chrono>
#include <string>

namespace subsonic {

// 一行一请求的访问日志（RAII）。
//
// 成功也打，失败也打：客户端只会告诉用户「加载失败」，日志是唯一能重建
// 「客户端到底发了什么」的东西。析构时统一落行，因此任何 handler 提前
// return 都不会漏记（没调 done() 就按 failed 记，code=0）。
//
// 字段约定（顺序固定，便于 grep / awk）：
//   [subsonic] <METHOD> method=<m> status=<ok|failed> code=<n> ms=<n>
//              client=<c> v=<v> f=<f> [id=<id>] [size=<n>] [offset=<n>]
//              [err=<snake_case>]
//
// 永不打印 u / t / s / p（Subsonic 的凭据就在 query 里）。
class AccessLog {
public:
  AccessLog(const crow::request &req, std::string method, bool enabled);
  ~AccessLog();

  AccessLog(const AccessLog &) = delete;
  AccessLog &operator=(const AccessLog &) = delete;

  // 客户端回传的实体 id；超过 64 字符截断。
  void set_id(const std::string &id);
  // 附加的分页参数，如 "size=20 offset=40"。空串不打印。
  void set_paging(const std::string &text);

  // 请求结束。code = 0 表示成功，其余为 Subsonic 错误码。
  // error_name 是可选的内部错误名（ErrorCode 的 snake_case），失败时更容易定位。
  void done(int code, const std::string &error_name = std::string());

private:
  void flush();

  bool enabled_ = true;
  bool finished_ = false;
  bool failed_ = false;
  bool flushed_ = false;
  std::string method_;
  std::string client_;
  std::string version_;
  std::string format_;
  std::string id_;
  std::string paging_;
  std::string error_name_;
  int code_ = 0;
  std::chrono::steady_clock::time_point started_;
};

} // namespace subsonic

#endif // SUBSONIC_ACCESS_LOG_H
