#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace webdav {

/// 解析后的 WebDAV URL（支持在配置中指定子路径作为媒体库根）。
struct Url {
  bool https = false;
  std::string host;
  std::string port;
  std::string user;
  std::string pass;
  std::string path; // 服务器绝对路径，以 '/' 开头，已百分号编码
};

/// 解析 `http(s)://[user:pass@]host[:port][/path]`；成功返回 true。
bool parseUrl(const std::string &url, Url &out);

/// 判断字符串是否为 http/https URL。
bool isWebdavUrl(const std::string &url);

/// 对单个路径片段做百分号编码（保留 '/' 之外的安全字符）。
std::string encodePathSegment(const std::string &segment);

/// 把 base（含子路径，通常以 '/' 结尾）与相对名拼接为服务器绝对路径。
std::string joinPath(const std::string &base_path, const std::string &name);

/// 由 base 的 scheme/host/port 与已编码路径拼出完整 URL。
std::string buildUrl(const Url &base, const std::string &encoded_path);

struct Response {
  int status = 0;
  std::string body;
  std::map<std::string, std::string> headers;
  std::string header(const std::string &key) const;
};

/// 流式读取句柄：保持与远端的连接，逐块读取响应体（不整块进内存）。
struct Stream {
  std::function<long long(char *buf, std::size_t n)> read;
  std::function<void()> close;
  int status = 0;
  long long length = -1;
  std::map<std::string, std::string> headers;
};

/// 打开一个 GET 流（支持 Range 透传）。失败时 status 为 0。
Stream openStream(const Url &base, const std::string &path,
                  const std::vector<std::string> &extra_headers);

/// 同步 HTTP/1.1 客户端（asio + OpenSSL，Basic 认证，Connection: close）。
class Client {
public:
  explicit Client(const Url &base) : base_(base) {}

  const Url &base() const { return base_; }

  Response request(const std::string &method, const std::string &path,
                   const std::vector<std::string> &extra_headers,
                   const std::string &body);

  Response head(const std::string &path);
  Response getRange(const std::string &path, long long offset, long long length);
  Response getAll(const std::string &path);
  Response put(const std::string &path, const std::string &data);
  Response move(const std::string &from, const std::string &to);
  Response del(const std::string &path);
  Response propfind(const std::string &path, int depth);
  long long contentLength(const std::string &path);

private:
  Url base_;
};

struct Resource {
  std::string path; // 服务器绝对路径（保持百分号编码，可直接用于 Client 请求）
  bool collection = false;
  long long size = 0;
  std::string last_modified;
};

/// 解析 PROPFIND 207 XML 响应。
std::vector<Resource> parsePropfind(const std::string &xml);

/// 从 URL 字符串中按 host 匹配配置里的凭据（找不到则用 URL 内嵌 userinfo）。
Url resolveCredentials(const std::string &url);

} // namespace webdav
