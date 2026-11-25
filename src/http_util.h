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

} // namespace http_util

#endif