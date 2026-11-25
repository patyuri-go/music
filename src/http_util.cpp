#include "http_util.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

#include "api/webdav/webdav.h"
#include "image_cache.h"
#include "storage.h"

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

ResponsePtr serveResource(const std::string &location,
                          const std::string &req_range) {
  if (!webdav::isWebdavUrl(location)) {
    return serveFile(fs::path(location), req_range);
  }

  webdav::Url u = webdav::resolveCredentials(location);
  std::vector<std::string> extra;
  if (!req_range.empty()) {
    extra.push_back("Range: " + req_range);
  }
  webdav::Stream s = webdav::openStream(u, u.path, extra);
  if (s.status != 200 && s.status != 206) {
    std::cerr << "[res] open error url=" << location << " status=" << s.status
              << " range=[" << req_range << "]" << std::endl;
    return jsonResponse(s.status == 0 ? 502 : s.status, "{\"erro\":true}");
  }
  std::cerr << "[res] open url=" << location << " status=" << s.status
            << " len=" << s.length << " range=[" << req_range << "]"
            << std::endl;

  const auto cr_it = s.headers.find("content-range");
  const std::string cr =
      cr_it == s.headers.end() ? std::string() : cr_it->second;
  // 长度未知整块缓冲
  if (s.length < 0) {
    std::string body;
    char buf[65536];
    while (true) {
      const long long n = s.read(buf, sizeof(buf));
      if (n <= 0) {
        break;
      }
      body.append(buf, static_cast<std::size_t>(n));
    }
    s.close();
    std::cerr << "[res] buffered url=" << location << " bytes=" << body.size()
              << std::endl;

    ResponsePtr res = drogon::HttpResponse::newHttpResponse();
    res->setStatusCode(static_cast<drogon::HttpStatusCode>(s.status));
    res->setContentTypeString(mimeType(fs::path(u.path)));
    res->addHeader("Accept-Ranges", "bytes");
    if (!cr.empty()) {
      res->addHeader("Content-Range", cr);
    }
    res->addHeader("Content-Length", std::to_string(body.size()));
    res->setBody(std::move(body));
    return res;
  }

  auto stream = std::make_shared<webdav::Stream>(std::move(s));
  auto sent = std::make_shared<long long>(0);
  const long long declared_length = stream->length;

  ResponsePtr res = drogon::HttpResponse::newStreamResponse(
      [stream, sent, location](char *buf, std::size_t len) -> std::size_t {
        // buf == nullptr 表示响应已完成或连接中断：关闭远端流。
        if (buf == nullptr) {
          stream->close();
          return 0;
        }
        const long long n = stream->read(buf, len);
        if (n <= 0) {
          std::cerr << "[res] done url=" << location << " sent=" << *sent
                    << std::endl;
          return 0;
        }
        *sent += n;
        return static_cast<std::size_t>(n);
      },
      "", drogon::CT_NONE, mimeType(fs::path(u.path)));
  res->setStatusCode(static_cast<drogon::HttpStatusCode>(stream->status));
  if (declared_length > 0) {
    res->addHeader("Content-Length", std::to_string(declared_length));
  }
  res->addHeader("Accept-Ranges", "bytes");
  if (!cr.empty()) {
    res->addHeader("Content-Range", cr);
  }
  return res;
}

ResponsePtr serveImage(const std::string &virtual_path,
                       const std::string &variant) {
  if (auto cache = image_cache::get()) {
    std::string error;
    if (const auto hit = cache->ensure(virtual_path, variant, error)) {
      return serveFile(*hit, "");
    }
    LOG_DEBUG << "[image_cache] ensure failed for " << virtual_path << ": "
              << error;
  }
  const std::string location = storage::resolve(virtual_path);
  if (!webdav::isWebdavUrl(location) &&
      !storage::isUnderAnyRoot(fs::path(location))) {
    return jsonResponse(404, "{\"erro\":true}");
  }
  return serveResource(location, "");
}

} // namespace http_util