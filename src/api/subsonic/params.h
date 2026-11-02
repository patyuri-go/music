#ifndef SUBSONIC_PARAMS_H
#define SUBSONIC_PARAMS_H

#include "crow/http_request.h"

#include <optional>
#include <string>

namespace subsonic {

// Subsonic 的查询参数读取。
//
// 与 Jellyfin 门面的区别：Subsonic 的参数名是固定小写，但客户端实现会大小写
// 混用，这里统一做大小写不敏感匹配。凭据参数（u / t / s / p）也在这里读，
// 但只用于「存在性」判断——本服务不做账号校验，绝不落日志。
std::string text(const crow::request &req, const char *name);

// 带缺省值的整数读取；解析失败按缺省值处理。
int integer(const crow::request &req, const char *name, int fallback);

// 分页：Subsonic 用 `size` / `offset` 两个数字，内部用不透明字符串游标。
// offset 直接当游标用（本地库的游标就是十进制偏移）。
struct Paging {
  int size = 50;
  int offset = 0;

  // clamp 到合法范围，防止单请求把聚合列表拉穿。
  void clamp(int max_size = 500, int max_offset = 100000);
  // "size=20 offset=0"，写访问日志用。
  std::string log_text() const;
};

Paging paging(const crow::request &req, int default_size);

// 实体 id：缺省 / 空 -> nullopt。
std::optional<std::string> id_param(const crow::request &req);

} // namespace subsonic

#endif // SUBSONIC_PARAMS_H
