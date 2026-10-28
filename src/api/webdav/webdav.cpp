#define ASIO_STANDALONE

#include "webdav.h"

#include "config.h"
#include "db.h"
#include "schema.h"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>

#include <sys/socket.h>
#include <sys/time.h>

namespace webdav {
namespace {

constexpr const char *kB64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string lower(std::string s) {
  for (char &c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

std::string base64(const std::string &in) {
  std::string out;
  int val = 0;
  int bits = -6;
  for (const unsigned char c : in) {
    val = (val << 8) + c;
    bits += 8;
    while (bits >= 0) {
      out.push_back(kB64[(val >> bits) & 0x3F]);
      bits -= 6;
    }
  }
  if (bits > -6) {
    out.push_back(kB64[((val << 8) >> (bits + 8)) & 0x3F]);
  }
  while (out.size() % 4 != 0) {
    out.push_back('=');
  }
  return out;
}

bool isUnreserved(unsigned char c) {
  return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~';
}

std::string percentDecode(const std::string &s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = std::isxdigit(static_cast<unsigned char>(s[i + 1]))
                         ? std::stoi(s.substr(i + 1, 2), nullptr, 16)
                         : -1;
      if (hi >= 0) {
        out.push_back(static_cast<char>(hi));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

std::string trim(const std::string &s) {
  const std::size_t b = s.find_first_not_of(" \t\r\n");
  const std::size_t e = s.find_last_not_of(" \t\r\n");
  return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
}

std::string xmlUnescape(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out += s[i];
      continue;
    }
    const std::size_t semi = s.find(';', i + 1);
    if (semi == std::string::npos || semi - i > 12) {
      out += s[i];
      continue;
    }
    const std::string ent = s.substr(i + 1, semi - i - 1);
    if (ent == "amp") {
      out += '&';
    } else if (ent == "lt") {
      out += '<';
    } else if (ent == "gt") {
      out += '>';
    } else if (ent == "quot") {
      out += '"';
    } else if (ent == "apos") {
      out += '\'';
    } else if (ent.size() > 1 && ent[0] == '#') {
      long code = 0;
      bool ok = true;
      if (ent[1] == 'x' || ent[1] == 'X') {
        if (ent.size() == 2) { ok = false; }
        for (std::size_t k = 2; ok && k < ent.size(); ++k) {
          const char c = ent[k];
          if (c >= '0' && c <= '9') { code = code * 16 + (c - '0'); }
          else if (c >= 'a' && c <= 'f') { code = code * 16 + (c - 'a' + 10); }
          else if (c >= 'A' && c <= 'F') { code = code * 16 + (c - 'A' + 10); }
          else { ok = false; }
        }
      } else {
        for (std::size_t k = 1; ok && k < ent.size(); ++k) {
          const char c = ent[k];
          if (c >= '0' && c <= '9') { code = code * 10 + (c - '0'); }
          else { ok = false; }
        }
      }
      if (!ok || code <= 0 || code > 0x10FFFF) {
        out += s.substr(i, semi - i + 1);
      } else {
        // UTF-8 encode code point
        if (code < 0x80) {
          out += static_cast<char>(code);
        } else if (code < 0x800) {
          out += static_cast<char>(0xC0 | (code >> 6));
          out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
          out += static_cast<char>(0xE0 | (code >> 12));
          out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
          out += static_cast<char>(0xF0 | (code >> 18));
          out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
          out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (code & 0x3F));
        }
      }
    } else {
      out += s.substr(i, semi - i + 1);
    }
    i = semi;
  }
  return out;
}

std::string extractTag(const std::string &block, const std::string &name) {
  std::size_t pos = 0;
  while (true) {
    const std::size_t at = block.find(name, pos);
    if (at == std::string::npos) {
      return {};
    }
    const std::size_t open = block.find('>', at);
    if (open == std::string::npos) {
      return {};
    }
    if (open > 0 && block[open - 1] == '/') { // self-closing tag
      pos = open + 1;
      continue;
    }
    const std::size_t close = block.find('<', open + 1);
    if (close == std::string::npos) {
      return {};
    }
    return block.substr(open + 1, close - open - 1);
  }
}

void setTimeout(asio::ip::tcp::socket &sock, int seconds) {
  struct timeval tv{};
  tv.tv_sec = seconds;
  setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

template <typename SyncStream>
std::string readAll(SyncStream &stream) {
  std::string out;
  char buf[16384];
  std::error_code ec;
  for (;;) {
    const std::size_t n = stream.read_some(asio::buffer(buf, sizeof(buf)), ec);
    if (n > 0) {
      out.append(buf, n);
    }
    if (ec) {
      break;
    }
  }
  return out;
}

std::string dechunk(const std::string &in) {
  std::string out;
  std::size_t pos = 0;
  while (pos < in.size()) {
    const std::size_t eol = in.find("\r\n", pos);
    if (eol == std::string::npos) {
      break;
    }
    const long n = std::strtol(in.substr(pos, eol - pos).c_str(), nullptr, 16);
    if (n <= 0) {
      break;
    }
    pos = eol + 2;
    out.append(in, pos, static_cast<std::size_t>(n));
    pos += static_cast<std::size_t>(n) + 2;
  }
  return out;
}

Response parseResponse(const std::string &raw) {
  Response res;
  const std::size_t head_end = raw.find("\r\n\r\n");
  if (head_end == std::string::npos) {
    res.status = 0;
    return res;
  }
  std::istringstream head(raw.substr(0, head_end));
  std::string line;
  std::getline(head, line);
  {
    const std::size_t sp = line.find(' ');
    if (sp != std::string::npos) {
      res.status = std::atoi(line.substr(sp + 1).c_str());
    }
  }
  while (std::getline(head, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) {
      res.headers[lower(trim(line.substr(0, colon)))] =
          trim(line.substr(colon + 1));
    }
  }
  std::string body = raw.substr(head_end + 4);
  const auto te = res.headers.find("transfer-encoding");
  if (te != res.headers.end() && lower(te->second).find("chunked") != std::string::npos) {
    body = dechunk(body);
  }
  res.body = std::move(body);
  return res;
}

template <typename SyncStream>
Response doExchange(SyncStream &stream, const std::string &request) {
  std::error_code ec;
  asio::write(stream, asio::buffer(request), ec);
  if (ec) {
    return Response{};
  }
  return parseResponse(readAll(stream));
}

} // namespace

bool isWebdavUrl(const std::string &url) {
  const std::string l = lower(url);
  return l.rfind("http://", 0) == 0 || l.rfind("https://", 0) == 0;
}

bool parseUrl(const std::string &url, Url &out) {
  const std::size_t p = url.find("://");
  if (p == std::string::npos) {
    return false;
  }
  const std::string scheme = lower(url.substr(0, p));
  if (scheme == "https") {
    out.https = true;
  } else if (scheme == "http") {
    out.https = false;
  } else {
    return false;
  }
  const std::string rest = url.substr(p + 3);
  const std::size_t slash = rest.find('/');
  std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  std::string path = (slash == std::string::npos) ? "/" : rest.substr(slash);

  const std::size_t at = authority.find('@');
  if (at != std::string::npos) {
    const std::string userinfo = authority.substr(0, at);
    authority = authority.substr(at + 1);
    const std::size_t colon = userinfo.find(':');
    if (colon == std::string::npos) {
      out.user = percentDecode(userinfo);
    } else {
      out.user = percentDecode(userinfo.substr(0, colon));
      out.pass = percentDecode(userinfo.substr(colon + 1));
    }
  }

  if (!authority.empty() && authority.front() == '[') {
    const std::size_t close = authority.find(']');
    out.host = authority.substr(0, close + 1);
    if (close + 1 < authority.size() && authority[close + 1] == ':') {
      out.port = authority.substr(close + 2);
    }
  } else {
    const std::size_t colon = authority.find(':');
    if (colon == std::string::npos) {
      out.host = authority;
    } else {
      out.host = authority.substr(0, colon);
      out.port = authority.substr(colon + 1);
    }
  }
  if (out.host.empty()) {
    return false;
  }
  out.path = path;
  return true;
}

std::string encodePathSegment(const std::string &segment) {
  std::string out;
  for (const unsigned char c : segment) {
    if (isUnreserved(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      char buf[4];
      std::snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

std::string joinPath(const std::string &base_path, const std::string &name) {
  std::string base = base_path;
  if (base.empty()) {
    base = "/";
  }
  if (base.back() != '/') {
    base += '/';
  }
  return base + encodePathSegment(name);
}

std::string buildUrl(const Url &base, const std::string &encoded_path) {
  std::string out = base.https ? "https://" : "http://";
  out += base.host;
  if (!base.port.empty()) {
    out += ':';
    out += base.port;
  }
  out += encoded_path.empty() ? "/" : encoded_path;
  return out;
}

std::string Response::header(const std::string &key) const {
  const auto it = headers.find(lower(key));
  return it == headers.end() ? std::string() : it->second;
}

namespace {

Response doRequest(const Url &target, const std::string &method,
                   const std::string &path,
                   const std::vector<std::string> &extra_headers,
                   const std::string &body, bool send_auth) {
  asio::io_context ioc;
  asio::ip::tcp::resolver resolver(ioc);
  const std::string port =
      target.port.empty() ? (target.https ? "443" : "80") : target.port;

  std::error_code ec;
  auto results = resolver.resolve(target.host, port, ec);
  if (ec) {
    return Response{};
  }

  std::string safe_path = path;
  for (std::size_t i = 0; i < safe_path.size(); ++i) {
    if (safe_path[i] == ' ') {
      safe_path.replace(i, 1, "%20");
      i += 2;
    }
  }
  std::string req = method + " " + safe_path + " HTTP/1.1\r\n";
  req += "Host: " + target.host + "\r\n";
  if (send_auth && (!target.user.empty() || !target.pass.empty())) {
    req += "Authorization: Basic " + base64(target.user + ":" + target.pass) +
           "\r\n";
  }
  req += "User-Agent: musicd-webdav/1.0\r\n";
  req += "Accept: */*\r\n";
  for (const auto &h : extra_headers) {
    req += h + "\r\n";
  }
  if (!body.empty() || method == "PUT" || method == "POST") {
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  req += "Connection: close\r\n\r\n";
  req += body;

  if (target.https) {
    asio::ssl::context ctx(asio::ssl::context::tls_client);
    ctx.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(ioc, ctx);
    asio::connect(stream.next_layer(), results, ec);
    if (ec) {
      return Response{};
    }
    setTimeout(stream.next_layer(), 30);
    stream.handshake(asio::ssl::stream_base::client, ec);
    if (ec) {
      return Response{};
    }
    return doExchange(stream, req);
  }

  asio::ip::tcp::socket sock(ioc);
  asio::connect(sock, results, ec);
  if (ec) {
    return Response{};
  }
  setTimeout(sock, 30);
  return doExchange(sock, req);
}

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 ||
         status == 308;
}

} // namespace

Response Client::request(const std::string &method, const std::string &path,
                         const std::vector<std::string> &extra_headers,
                         const std::string &body) {
  Response resp =
      doRequest(base_, method, path, extra_headers, body, true);
  int redirects = 0;
  while (isRedirect(resp.status) && redirects < 5) {
    const std::string loc = resp.header("location");
    if (loc.empty()) {
      break;
    }
    Url next = base_;
    std::string next_path = loc;
    if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0) {
      const std::size_t scheme_end = loc.find("://");
      const std::size_t slash = loc.find('/', scheme_end + 3);
      std::string authority = (slash == std::string::npos)
                                  ? loc.substr(scheme_end + 3)
                                  : loc.substr(scheme_end + 3,
                                               slash - (scheme_end + 3));
      next_path = (slash == std::string::npos) ? "/" : loc.substr(slash);
      next.https = loc.rfind("https://", 0) == 0;
      next.user.clear();
      next.pass.clear();
      const std::size_t at = authority.rfind('@');
      if (at != std::string::npos) {
        authority = authority.substr(at + 1);
      }
      if (!authority.empty() && authority.front() == '[') {
        const std::size_t close = authority.find(']');
        next.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') {
          next.port = authority.substr(close + 2);
        } else {
          next.port.clear();
        }
      } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
          next.host = authority.substr(0, colon);
          next.port = authority.substr(colon + 1);
        } else {
          next.host = authority;
          next.port.clear();
        }
      }
    }
    const bool same = (next.host == base_.host && next.port == base_.port &&
                       next.https == base_.https);
    resp = doRequest(next, method, next_path, extra_headers, body, same);
    ++redirects;
  }
  return resp;
}

Response Client::head(const std::string &path) {
  return request("HEAD", path, {}, {});
}

Response Client::getRange(const std::string &path, long long offset,
                          long long length) {
  std::vector<std::string> h;
  if (offset > 0 || length >= 0) {
    std::string range = "Range: bytes=" + std::to_string(offset) + "-";
    if (length >= 0) {
      range += std::to_string(offset + length - 1);
    }
    h.push_back(range);
  }
  return request("GET", path, h, {});
}

Response Client::getAll(const std::string &path) {
  return request("GET", path, {}, {});
}

Response Client::put(const std::string &path, const std::string &data) {
  return request("PUT", path, {}, data);
}

Response Client::move(const std::string &from, const std::string &to) {
  return request("MOVE", from, {"Destination: " + to, "Overwrite: T"}, {});
}

Response Client::del(const std::string &path) {
  return request("DELETE", path, {}, {});
}

Response Client::propfind(const std::string &path, int depth) {
  return request("PROPFIND", path,
                 {"Depth: " + std::to_string(depth),
                  "Content-Type: application/xml"},
                 "<?xml version=\"1.0\"?><D:propfind "
                 "xmlns:D=\"DAV:\"><D:prop><D:resourcetype/>"
                 "<D:getcontentlength/><D:getlastmodified/></D:prop>"
                 "</D:propfind>");
}

std::optional<Metadata> Client::probe(const std::string &path) {
  auto nonNegative = [](const std::string &value) -> long long {
    if (value.empty()) {
      return -1;
    }
    char *end = nullptr;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end == value.c_str() || parsed < 0) {
      return -1;
    }
    return parsed;
  };

  Metadata metadata;
  const Response head_response = head(path);
  if (head_response.status >= 200 && head_response.status < 300) {
    metadata.length = nonNegative(head_response.header("content-length"));
    metadata.last_modified = head_response.header("last-modified");
    metadata.content_type = head_response.header("content-type");
    metadata.etag = head_response.header("etag");
    if (metadata.length >= 0) {
      return metadata;
    }
  }

  Stream stream = openStream(base_, path, {"Range: bytes=0-0"});
  if (stream.status != 200 && stream.status != 206) {
    if (stream.close) {
      stream.close();
    }
    return std::nullopt;
  }
  auto streamHeader = [&stream](const char *key) {
    const auto it = stream.headers.find(key);
    return it == stream.headers.end() ? std::string() : it->second;
  };
  metadata.last_modified = streamHeader("last-modified");
  metadata.content_type = streamHeader("content-type");
  metadata.etag = streamHeader("etag");
  if (stream.status == 206) {
    const std::string range = streamHeader("content-range");
    const std::size_t slash = range.find('/');
    if (slash != std::string::npos) {
      metadata.length = nonNegative(range.substr(slash + 1));
    }
  } else {
    metadata.length = nonNegative(streamHeader("content-length"));
  }
  if (stream.close) {
    stream.close();
  }
  if (metadata.length < 0) {
    return std::nullopt;
  }
  return metadata;
}

long long Client::contentLength(const std::string &path) {
  Response h = head(path);
  if (h.status >= 200 && h.status < 300) {
    const std::string cl = h.header("content-length");
    if (!cl.empty()) {
      return std::strtoll(cl.c_str(), nullptr, 10);
    }
  }
  Response r = getRange(path, 0, 1);
  if (r.status == 206) {
    const std::string cr = r.header("content-range"); // bytes 0-0/1234
    const std::size_t slash = cr.find('/');
    if (slash != std::string::npos) {
      return std::strtoll(cr.substr(slash + 1).c_str(), nullptr, 10);
    }
  }
  return -1;
}

namespace {

std::string buildRequestString(const Url &target, const std::string &method,
                               const std::string &path,
                               const std::vector<std::string> &extra_headers,
                               const std::string &body, bool send_auth) {
  std::string safe_path = path;
  for (std::size_t i = 0; i < safe_path.size(); ++i) {
    if (safe_path[i] == ' ') {
      safe_path.replace(i, 1, "%20");
      i += 2;
    }
  }
  std::string req = method + " " + safe_path + " HTTP/1.1\r\n";
  req += "Host: " + target.host + "\r\n";
  if (send_auth && (!target.user.empty() || !target.pass.empty())) {
    req += "Authorization: Basic " + base64(target.user + ":" + target.pass) +
           "\r\n";
  }
  req += "User-Agent: musicd-webdav/1.0\r\n";
  req += "Accept: */*\r\n";
  for (const auto &h : extra_headers) {
    req += h + "\r\n";
  }
  if (!body.empty() || method == "PUT" || method == "POST") {
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  req += "Connection: close\r\n\r\n";
  req += body;
  return req;
}

void parseStreamHead(const std::string &head, Stream &s) {
  const std::size_t line_end = head.find("\r\n");
  const std::string status_line =
      head.substr(0, line_end == std::string::npos ? head.size() : line_end);
  const std::size_t sp = status_line.find(' ');
  if (sp != std::string::npos) {
    s.status = std::atoi(status_line.c_str() + sp + 1);
  }
  std::size_t pos = (line_end == std::string::npos) ? head.size() : line_end + 2;
  while (pos < head.size()) {
    const std::size_t e = head.find("\r\n", pos);
    if (e == std::string::npos) {
      break;
    }
    const std::string line = head.substr(pos, e - pos);
    if (line.empty()) {
      break;
    }
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) {
      s.headers[lower(trim(line.substr(0, colon)))] =
          trim(line.substr(colon + 1));
    }
    pos = e + 2;
  }
  const auto it = s.headers.find("content-length");
  if (it != s.headers.end()) {
    s.length = std::strtoll(it->second.c_str(), nullptr, 10);
  }
}

template <typename StreamT>
Stream openOnStream(std::shared_ptr<StreamT> stream,
                    std::shared_ptr<asio::io_context> ioc,
                    std::shared_ptr<asio::ssl::context> ctx,
                    const std::string &req) {
  std::error_code ec;
  asio::write(*stream, asio::buffer(req), ec);
  if (ec) {
    return Stream{};
  }
  auto pending = std::make_shared<std::string>();
  char tmp[16384];
  while (true) {
    const std::size_t got = stream->read_some(asio::buffer(tmp, sizeof(tmp)), ec);
    if (ec || got == 0) {
      return Stream{};
    }
    pending->append(tmp, got);
    if (pending->find("\r\n\r\n") != std::string::npos) {
      break;
    }
    if (pending->size() > 131072) {
      return Stream{};
    }
  }
  const std::size_t head_end = pending->find("\r\n\r\n");
  const std::string head = pending->substr(0, head_end + 2);
  *pending = pending->substr(head_end + 4);

  Stream s;
  parseStreamHead(head, s);
  s.read = [stream, ioc, ctx, pending](char *buf,
                                       std::size_t n) -> long long {
    if (!pending->empty()) {
      const std::size_t take = std::min(n, pending->size());
      std::memcpy(buf, pending->data(), take);
      pending->erase(0, take);
      return static_cast<long long>(take);
    }
    std::error_code read_ec;
    const std::size_t got = stream->read_some(asio::buffer(buf, n), read_ec);
    if (read_ec) {
      return -1;
    }
    return static_cast<long long>(got);
  };
  s.close = [stream, ioc, ctx]() {
    std::error_code close_ec;
    stream->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both,
                                    close_ec);
    stream->lowest_layer().close(close_ec);
  };
  return s;
}

} // namespace

Stream openStream(const Url &base, const std::string &path,
                  const std::vector<std::string> &extra_headers) {
  Url target = base;
  std::string req_path = path;
  bool send_auth = true;
  for (int attempt = 0; attempt < 6; ++attempt) {
    const std::string port =
        target.port.empty() ? (target.https ? "443" : "80") : target.port;
    auto ioc = std::make_shared<asio::io_context>();
    asio::ip::tcp::resolver resolver(*ioc);
    std::error_code ec;
    const auto results = resolver.resolve(target.host, port, ec);
    if (ec) {
      return Stream{};
    }
    const std::string req = buildRequestString(target, "GET", req_path,
                                               extra_headers, "", send_auth);
    Stream s;
    if (target.https) {
      auto ctx =
          std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
      ctx->set_verify_mode(asio::ssl::verify_none);
      auto stream =
          std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(*ioc, *ctx);
      asio::connect(stream->next_layer(), results, ec);
      if (ec) {
        return Stream{};
      }
      setTimeout(stream->next_layer(), 30);
      stream->handshake(asio::ssl::stream_base::client, ec);
      if (ec) {
        return Stream{};
      }
      s = openOnStream(stream, ioc, ctx, req);
    } else {
      auto stream = std::make_shared<asio::ip::tcp::socket>(*ioc);
      asio::connect(*stream, results, ec);
      if (ec) {
        return Stream{};
      }
      setTimeout(*stream, 30);
      s = openOnStream(stream, ioc, nullptr, req);
    }
    if (s.status == 0) {
      return Stream{};
    }
    if (isRedirect(s.status)) {
      const auto it = s.headers.find("location");
      const std::string location =
          it == s.headers.end() ? std::string() : it->second;
      if (s.close) {
        s.close();
      }
      if (location.empty()) {
        return Stream{};
      }
      Url next;
      if (location.rfind("http://", 0) == 0 ||
          location.rfind("https://", 0) == 0) {
        if (!parseUrl(location, next)) {
          return Stream{};
        }
        next.user.clear();
        next.pass.clear();
      } else {
        next = target;
        if (!location.empty() && location[0] == '/') {
          next.path = location;
        } else {
          const std::size_t slash = target.path.rfind('/');
          next.path =
              (slash == std::string::npos ? std::string("/")
                                          : target.path.substr(0, slash + 1)) +
              location;
        }
      }
      req_path = next.path;
      target = next;
      send_auth = (next.host == base.host && next.port == base.port &&
                   next.https == base.https);
      continue;
    }
    return s;
  }
  return Stream{};
}

std::vector<Resource> parsePropfind(const std::string &xml) {
  std::vector<Resource> out;
  std::size_t pos = 0;
  while (true) {
    const std::size_t at = xml.find("response", pos);
    if (at == std::string::npos) {
      break;
    }
    const std::size_t open = xml.find('>', at);
    if (open == std::string::npos) {
      break;
    }
    std::size_t close = xml.find("</D:response>", open + 1);
    if (close == std::string::npos) {
      close = xml.find("</response>", open + 1);
    }
    if (close == std::string::npos) {
      close = xml.find("</", open + 1);
      if (close == std::string::npos) {
        break;
      }
    }
    const std::string block = xml.substr(open + 1, close - open - 1);
    pos = close;

    Resource r;
    std::string href = xmlUnescape(trim(extractTag(block, "href")));
    if (href.empty()) {
      continue;
    }
    if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0) {
      const std::size_t sp = href.find("://");
      const std::size_t slash = href.find('/', sp + 3);
      href = (slash == std::string::npos) ? "/" : href.substr(slash);
    }
    if (href.size() > 1 && href.back() == '/') {
      href.pop_back();
    }
    r.path = href; // 保持百分号编码，与 Url.path / Client 请求路径一致
    r.collection = block.find("collection") != std::string::npos;
    const std::string cl = trim(extractTag(block, "getcontentlength"));
    r.size = cl.empty() ? 0 : std::strtoll(cl.c_str(), nullptr, 10);
    r.last_modified = trim(extractTag(block, "getlastmodified"));
    out.push_back(std::move(r));
  }
  return out;
}

Url resolveCredentials(const std::string &url) {
  Url u;
  parseUrl(url, u);
  if (!u.user.empty() || !u.pass.empty()) {
    return u;
  }
  for (const auto &root : config::get().webdav_roots) {
    Url c;
    if (parseUrl(root.url, c) && c.host == u.host && c.port == u.port) {
      u.user = root.user;
      u.pass = root.pass;
      break;
    }
  }
  if (!u.user.empty() || !u.pass.empty()) {
    return u;
  }
  try {
    SQLite::Database &db = db::conn(config::get().db_path.string());
    SQLite::Statement q(db, schema::sql::kScanRootCredentials);
    std::size_t best_len = 0;
    while (q.executeStep()) {
      const std::string root_url = q.getColumn(0).getText();
      const std::string root_user = q.getColumn(1).getText();
      const std::string root_pass = q.getColumn(2).getText();
      Url c;
      if (!parseUrl(root_url, c) || c.host != u.host || c.port != u.port) {
        continue;
      }
      if (!c.path.empty() && u.path.rfind(c.path, 0) != 0) {
        continue;
      }
      if (c.path.size() >= best_len) {
        best_len = c.path.size();
        u.user = root_user;
        u.pass = root_pass;
      }
    }
  } catch (...) {
  }
  return u;
}

} // namespace webdav
