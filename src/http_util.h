#ifndef HTTP_UTIL_H
#define HTTP_UTIL_H

#include "http_server.h"

#include <filesystem>
#include <string>
#include <utility>

namespace http_util {
namespace fs = std::filesystem;

using http_server::RequestPtr;
using http_server::ResponsePtr;

std::string mimeType(const fs::path &p);
std::string urlDecode(const std::string &in);
ResponsePtr jsonResponse(int code, std::string body);
std::pair<std::streamoff, std::streamoff>
parseRange(const std::string &range_header, std::streamoff file_size);
ResponsePtr serveFile(const fs::path &path, const std::string &req_range);

/// 本地路径或 http(s) WebDAV URL 统一读取；URL 走远端 Range 透传代理。
ResponsePtr serveResource(const std::string &location,
                          const std::string &req_range);

/// 图片专用：按虚拟路径读取，优先走图片缓存；未命中回退到流式转发。
ResponsePtr serveImage(const std::string &virtual_path,
                       const std::string &variant = "original");

} // namespace http_util

#endif