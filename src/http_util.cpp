#include "http_util.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>


namespace http_util {

std::string mimeType(const fs::path &p) {
  const std::string ext = p.extension().string();
  if (ext == ".mp3")
    return "audio/mpeg";
  if (ext == ".flac")
    return "audio/flac";
  if (ext == ".ogg")
    return "audio/ogg";
  if (ext == ".m4a")
    return "audio/mp4";
  if (ext == ".opus")
    return "audio/opus";
  if (ext == ".wav")
    return "audio/wav";
  if (ext == ".aac")
    return "audio/aac";
  if (ext == ".wma")
    return "audio/x-ms-wma";
  if (ext == ".jpg" || ext == ".jpeg")
    return "image/jpeg";
  if (ext == ".png")
    return "image/png";
  if (ext == ".webp")
    return "image/webp";
  if (ext == ".html" || ext == ".htm")
    return "text/html";
  if (ext == ".css")
    return "text/css";
  if (ext == ".js" || ext == ".mjs")
    return "application/javascript";
  if (ext == ".json")
    return "application/json";
  if (ext == ".svg")
    return "image/svg+xml";
  if (ext == ".woff2")
    return "font/woff2";
  if (ext == ".woff")
    return "font/woff";
  if (ext == ".ttf")
    return "font/ttf";
  if (ext == ".ico")
    return "image/x-icon";
  return "application/octet-stream";
}

std::string urlDecode(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(in[i + 1]);
      const int lo = hex(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(in[i]);
  }
  return out;
}

ResponsePtr jsonResponse(int code, std::string body) {
  return http_server::textResponse(code, std::move(body),
                                   "application/json; charset=utf-8");
}

std::pair<std::streamoff, std::streamoff>
parseRange(const std::string &range_header, std::streamoff file_size) {
  if (file_size == 0) {
    return {0, -1};
  }
  const std::string prefix = "bytes=";
  if (range_header.size() <= prefix.size() ||
      range_header.substr(0, prefix.size()) != prefix) {
    return {0, file_size - 1};
  }
  const std::string range_str = range_header.substr(prefix.size());

  if (range_str[0] == '-') {
    std::streamoff suffix = std::stoll(range_str.substr(1));
    if (suffix <= 0 || suffix > file_size) {
      suffix = file_size;
    }
    return {file_size - suffix, file_size - 1};
  }

  const size_t dash = range_str.find('-');
  if (dash == std::string::npos) {
    return {0, file_size - 1};
  }

  std::streamoff start = std::stoll(range_str.substr(0, dash));
  std::streamoff end;
  if (dash + 1 >= range_str.size()) {
    end = file_size - 1;
  } else {
    end = std::stoll(range_str.substr(dash + 1));
  }

  if (start >= file_size || end >= file_size || start > end) {
    return {-1, -1};
  }
  return {start, end};
}

ResponsePtr serveFile(const fs::path &path, const std::string &req_range) {
  std::error_code ec;
  if (!fs::exists(path, ec) || ec) {
    return jsonResponse(404, "{\"erro\":true}");
  }
  const std::streamoff file_size =
      static_cast<std::streamoff>(fs::file_size(path, ec));
  if (ec) {
    return jsonResponse(404, "{\"erro\":true}");
  }

  auto [start, end] = parseRange(req_range, file_size);

  if (start == -1) {
    ResponsePtr res = http_server::textResponse(416, "range not satisfiable");
    res->addHeader("Content-Range", "bytes */" + std::to_string(file_size));
    return res;
  }

  const std::streamoff length = end - start + 1;
  const bool partial = !(start == 0 && end == file_size - 1);

  // Drogon 的区间文件响应：内部按 offset/length 读，越界自动 416。
  ResponsePtr res = drogon::HttpResponse::newFileResponse(
      path.string(), static_cast<std::size_t>(start),
      static_cast<std::size_t>(length), /*setContentRange=*/true, "",
      drogon::CT_NONE, mimeType(path), nullptr);
  res->setStatusCode(static_cast<drogon::HttpStatusCode>(partial ? 206 : 200));
  res->addHeader("Accept-Ranges", "bytes");
  return res;
}

} // namespace http_util