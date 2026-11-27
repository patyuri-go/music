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

} // namespace http_util