#ifndef MUSICD_STORAGE_CLIENT_H
#define MUSICD_STORAGE_CLIENT_H

#include "http_server.h"

#include <cstdint>
#include <functional>
#include <string>

namespace storage_client {

// 外部存储服务的连接参数。
//
// 该服务只做一件事：给定实体 id，返回内容字节流（见
// document/STORAGE_PROXY_DECISIONS.md）。本进程不解析它的数据库、不读本地
// 文件、不读 WebDAV。
struct Options {
  std::string host{"127.0.0.1"};
  int port{5666};
  std::string scheme{"http"};
  std::string token;
  int timeout_ms{5000};
  int max_concurrent{32};
  int workers{8};
};

void initialize(const Options &options);
void shutdown();
bool enabled();

// 上游只接受标准 UUID；这里先做同样的校验，避免把非法 id 发出去。
bool valid_entity_id(const std::string &entity_id);

// 存储服务的内容地址；配置里的 host/port/scheme 决定前缀。
std::string content_url(const std::string &entity_id);

// 配置里的超时（毫秒）。多来源路由拿它约束「探测响应头」的等待时间。
int timeout_ms();

} // namespace storage_client

#endif // MUSICD_STORAGE_CLIENT_H