#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <functional>
#include <map>
#include <string>
#include <vector>

// 轻量同步 HTTP 客户端（libcurl）。
//
// 只覆盖本进程真实用到的两件事：
//   1. 一次性请求（api-enhanced 的 JSON 接口）；
//   2. 顺序流式 GET（远端音频流）。
// WebDAV 专用能力（PROPFIND/MOVE 等）已删除；这里不依赖 asio/OpenSSL。
namespace http_client {

struct Url {
  bool https = false;
  std::string host;
  std::string port;
  std::string user;
  std::string pass;
  std::string path; // 已百分号编码的绝对路径，以 '/' 开头
};

// `http(s)://[user:pass@]host[:port][/path]`
bool parseUrl(const std::string &url, Url &out);

// 判断字符串是否为 http/https URL。
bool isHttpUrl(const std::string &url);

// 对单个路径片段做百分号编码（保留 unreserved 字符）。
std::string encodePathSegment(const std::string &segment);

// 由 base 的 scheme/host/port 与已编码路径拼出完整 URL。
std::string buildUrl(const Url &base, const std::string &encoded_path);

struct Response {
  long status = 0;
  std::string body;
  std::map<std::string, std::string> headers;
  std::string header(const std::string &key) const;
};

// 同步 HTTP/1.1 客户端。
class Client {
public:
  explicit Client(Url base) : base_(std::move(base)) {}

  const Url &base() const { return base_; }

  Response request(const std::string &method, const std::string &path,
                   const std::vector<std::string> &extra_headers,
                   const std::string &body);

private:
  Url base_;
};

// 流式读取句柄：保持连接，逐块读取响应体（不整块进内存）。
struct Stream {
  std::function<long long(char *buf, std::size_t n)> read;
  std::function<void()> close;
  int status = 0;
  long long length = -1;
  std::map<std::string, std::string> headers;
};

// 打开一个 GET 流（支持 Range 透传）。返回时响应头已就绪；失败时 status 为 0。
Stream openStream(const Url &base, const std::string &path,
                  const std::vector<std::string> &extra_headers);

} // namespace http_client

#endif // HTTP_CLIENT_H