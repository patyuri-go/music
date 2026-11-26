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

// 异步代理一次内容请求。
//
// - entity_id：track.entity_id（音频）或 album.entity_id（封面）的 UUID。
// - image：true 时用魔数识别封面类型，false 时用 TagLib 识别音频类型。
// - head_only：只回响应头，不建立流。
//
// callback 可能在工作线程上被调用。成功时返回的响应是
// newAsyncStreamResponse（chunked）；失败时是普通 JSON 响应。
using Callback = std::function<void(const http_server::ResponsePtr &)>;
void serve_content(const http_server::RequestPtr &req,
                   const std::string &entity_id, bool image, bool head_only,
                   Callback callback);

} // namespace storage_client

#endif // MUSICD_STORAGE_CLIENT_H