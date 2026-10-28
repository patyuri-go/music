#ifndef HTTP_UTIL_H
#define HTTP_UTIL_H

#include "crow/http_response.h"

#include <filesystem>
#include <string>
#include <utility>

namespace http_util {
namespace fs = std::filesystem;

std::string mimeType(const fs::path &p);
std::string urlDecode(const std::string &in);
crow::response jsonResponse(int code, std::string body);
std::pair<std::streamoff, std::streamoff>
parseRange(const std::string &range_header, std::streamoff file_size);
crow::response serveFile(const fs::path &path, const std::string &req_range);

/// 本地路径或 http(s) WebDAV URL 统一读取；URL 走远端 Range 透传代理。
crow::response serveResource(const std::string &location,
                             const std::string &req_range);

/// 图片专用：按虚拟路径读取，优先走图片缓存；未命中回退到流式转发。
crow::response serveImage(const std::string &virtual_path,
                          const std::string &variant = "original");

} // namespace http_util

#endif
