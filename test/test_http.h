#ifndef MUSICD_TEST_HTTP_H
#define MUSICD_TEST_HTTP_H

// 测试用的最小 HTTP/1.1 客户端。
//
// 只依赖 POSIX socket，不引入额外库；支持：
//   - 自定义请求头（Range 等）
//   - 读取到 EOF（服务端一律 Connection: close）
//   - 解析响应头（键统一小写）
//   - chunked 响应体解码（musicd 的异步流用 Transfer-Encoding: chunked）

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace test_http {

struct Response {
  int status = 0;
  std::map<std::string, std::string> headers;
  std::string body;

  std::string header(const std::string &key) const {
    std::string lower = key;
    for (char &c : lower) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const auto it = headers.find(lower);
    return it == headers.end() ? std::string() : it->second;
  }
};

inline std::string lower(std::string text) {
  for (char &c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

inline std::string trim(const std::string &text) {
  const std::size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  const std::size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

// 解码 chunked；遇到不完整数据时返回已解出的部分。
inline std::string decodeChunked(const std::string &raw) {
  std::string out;
  std::size_t pos = 0;
  while (pos < raw.size()) {
    const std::size_t line_end = raw.find("\r\n", pos);
    if (line_end == std::string::npos) {
      break;
    }
    const std::string size_text = raw.substr(pos, line_end - pos);
    char *end = nullptr;
    const unsigned long long size =
        std::strtoull(size_text.c_str(), &end, 16);
    if (end == size_text.c_str() || size == 0) {
      break;
    }
    const std::size_t data_begin = line_end + 2;
    if (data_begin + size > raw.size()) {
      out.append(raw, data_begin, raw.size() - data_begin);
      break;
    }
    out.append(raw, data_begin, static_cast<std::size_t>(size));
    pos = data_begin + static_cast<std::size_t>(size) + 2;
  }
  return out;
}

inline Response httpRequest(
    const std::string &host, int port, const std::string &method,
    const std::string &path, const std::string &body = std::string(),
    const std::vector<std::string> &extra_headers = {}) {
  Response resp;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return resp;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    return resp;
  }
  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return resp;
  }

  std::ostringstream req;
  req << method << " " << path << " HTTP/1.1\r\n";
  req << "Host: " << host << ":" << port << "\r\n";
  req << "Connection: close\r\n";
  for (const auto &header : extra_headers) {
    req << header << "\r\n";
  }
  if (!body.empty()) {
    req << "Content-Length: " << body.size() << "\r\n";
    req << "Content-Type: application/json\r\n";
  }
  req << "\r\n" << body;
  const std::string raw_req = req.str();
  std::size_t sent = 0;
  while (sent < raw_req.size()) {
    const ssize_t n =
        ::send(fd, raw_req.data() + sent, raw_req.size() - sent, 0);
    if (n <= 0) {
      ::close(fd);
      return resp;
    }
    sent += static_cast<std::size_t>(n);
  }

  std::string raw;
  char buf[16384];
  ssize_t n = 0;
  while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
    raw.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);

  const std::size_t head_end = raw.find("\r\n\r\n");
  if (head_end == std::string::npos) {
    return resp;
  }
  const std::string head = raw.substr(0, head_end);
  const std::string body_raw = raw.substr(head_end + 4);

  std::istringstream lines(head);
  std::string line;
  bool first = true;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (first) {
      const std::size_t sp = line.find(' ');
      if (sp != std::string::npos) {
        resp.status = std::atoi(line.c_str() + sp + 1);
      }
      first = false;
      continue;
    }
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) {
      resp.headers[lower(trim(line.substr(0, colon)))] =
          trim(line.substr(colon + 1));
    }
  }

  if (lower(resp.headers["transfer-encoding"]).find("chunked") !=
      std::string::npos) {
    resp.body = decodeChunked(body_raw);
  } else {
    resp.body = body_raw;
  }
  return resp;
}

} // namespace test_http

#endif // MUSICD_TEST_HTTP_H