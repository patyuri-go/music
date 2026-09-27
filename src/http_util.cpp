#include "http_util.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "api/webdav/webdav.h"
#include "crow/stream_response.h"

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

crow::response jsonResponse(int code, std::string body) {
  crow::response res(code, std::move(body));
  res.set_header("Content-Type", "application/json; charset=utf-8");
  return res;
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

crow::response serveFile(const fs::path &path, const std::string &req_range) {
  if (!fs::exists(path)) {
    return jsonResponse(404, "{\"erro\":true}");
  }

  auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (!*file) {
    return crow::response(500, "cannot open file");
  }

  file->seekg(0, std::ios::end);
  const std::streamoff file_size = file->tellg();
  file->seekg(0, std::ios::beg);

  auto [start, end] = parseRange(req_range, file_size);

  if (start == -1) {
    crow::response res(416, "range not satisfiable");
    res.add_header("Content-Range", "bytes */" + std::to_string(file_size));
    return res;
  }

  const std::streamoff length = end - start + 1;
  file->seekg(start, std::ios::beg);

  auto remaining = std::make_shared<std::streamoff>(length);
  crow::response res = crow::streamedResponse(
      (start != 0 || end != file_size - 1) ? 206 : 200, mimeType(path), length,
      [file, remaining](std::string &out) -> bool {
        if (*remaining <= 0) {
          return false;
        }
        const std::streamoff want = std::min<std::streamoff>(
            *remaining, static_cast<std::streamoff>(65536));
        out.resize(static_cast<std::size_t>(want));
        file->read(&out[0], static_cast<std::streamsize>(want));
        const std::streamsize got = file->gcount();
        if (got <= 0) {
          return false;
        }
        *remaining -= got;
        out.resize(static_cast<std::size_t>(got));
        return true;
      });
  res.add_header("Accept-Ranges", "bytes");
  res.add_header("Content-Range", "bytes " + std::to_string(start) + "-" +
                                      std::to_string(end) + "/" +
                                      std::to_string(file_size));
  return res;
}

crow::response serveResource(const std::string &location,
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

    crow::response res;
    res.code = s.status;
    res.add_header("Content-Type", mimeType(fs::path(u.path)));
    res.add_header("Accept-Ranges", "bytes");
    if (!cr.empty()) {
      res.add_header("Content-Range", cr);
    }
    res.add_header("Content-Length", std::to_string(body.size()));
    res.body = std::move(body);
    return res;
  }

  auto stream = std::make_shared<webdav::Stream>(std::move(s));
  auto sent = std::make_shared<long long>(0);
  crow::response res = crow::streamedResponse(
      stream->status, mimeType(fs::path(u.path)), stream->length,
      [stream, sent, location](std::string &out) -> bool {
        out.resize(65536);
        const long long n = stream->read(out.data(), out.size());
        if (n <= 0) {
          std::cerr << "[res] done url=" << location << " sent=" << *sent
                    << std::endl;
          return false;
        }
        *sent += n;
        out.resize(static_cast<std::size_t>(n));
        return true;
      },
      [stream]() { stream->close(); });
  res.add_header("Accept-Ranges", "bytes");
  if (!cr.empty()) {
    res.add_header("Content-Range", cr);
  }
  return res;
}

crow::response serveImage(const std::string &location) {
  // 原图直接流式转发：不缓存、不整块读入内存（本地与 WebDAV 均分块）。
  return serveResource(location, "");
}

} // namespace http_util
