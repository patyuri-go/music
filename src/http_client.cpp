#include "http_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace http_client {
namespace {

std::string lower(std::string text) {
  for (char &ch : text) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return text;
}

std::string trim(const std::string &text) {
  const std::size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  const std::size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

bool isUnreserved(unsigned char c) {
  return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~';
}

std::string hexByte(unsigned char c) {
  static const char *kDigits = "0123456789ABCDEF";
  std::string out;
  out.push_back('%');
  out.push_back(kDigits[c >> 4]);
  out.push_back(kDigits[c & 0x0F]);
  return out;
}

std::string origin(const Url &url) {
  std::string out = url.https ? "https://" : "http://";
  out += url.host;
  if (!url.port.empty()) {
    out.push_back(':');
    out += url.port;
  }
  return out;
}

void setCommonOptions(CURL *handle, const Url &base) {
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 30L);
  if (!base.user.empty()) {
    curl_easy_setopt(handle, CURLOPT_USERNAME, base.user.c_str());
    curl_easy_setopt(handle, CURLOPT_PASSWORD, base.pass.c_str());
  }
}

std::size_t header_cb(char *buffer, std::size_t size, std::size_t count,
                      void *user) {
  const std::size_t total = size * count;
  auto *headers = static_cast<std::map<std::string, std::string> *>(user);
  std::string line(buffer, total);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  const std::size_t colon = line.find(':');
  if (colon != std::string::npos) {
    (*headers)[lower(trim(line.substr(0, colon)))] =
        trim(line.substr(colon + 1));
  }
  return total;
}

std::size_t append_cb(char *buffer, std::size_t size, std::size_t count,
                      void *user) {
  auto *body = static_cast<std::string *>(user);
  body->append(buffer, size * count);
  return size * count;
}

// 一次流式连接的共享状态。生产者是 libcurl，消费者是 ByteStream。
struct SharedStream {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::string> chunks;
  bool headers_ready = false;
  bool finished = false;
  bool cancelled = false;
  bool failed = false;
  long status = 0;
  long long length = -1;
  std::map<std::string, std::string> headers;
  int error = 0;
  bool timed_out = false;

  void cancel() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      cancelled = true;
    }
    cv.notify_all();
  }
};

std::size_t stream_appendix_cb(char *buffer, std::size_t size,
                               std::size_t count, void *user) {
  auto *shared = static_cast<SharedStream *>(user);
  const std::size_t total = size * count;
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (shared->cancelled) {
      return 0;
    }
    shared->chunks.emplace_back(buffer, total);
  }
  shared->cv.notify_all();
  return total;
}

std::size_t stream_header_cb(char *buffer, std::size_t size,
                             std::size_t count, void *user) {
  auto *shared = static_cast<SharedStream *>(user);
  const std::size_t total = size * count;
  std::string line(buffer, total);
  const bool blank = line == "\r\n" || line == "\n";

  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (line.rfind("HTTP/", 0) == 0) {
      // 新一次响应（重定向/100 Continue）重置元数据。
      shared->headers.clear();
      shared->length = -1;
      const std::size_t space = line.find(' ');
      if (space != std::string::npos) {
        shared->status = std::strtol(line.c_str() + space + 1, nullptr, 10);
      }
    } else {
      const std::size_t colon = line.find(':');
      if (colon != std::string::npos) {
        const std::string key = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        shared->headers[key] = value;
        if (key == "content-length") {
          try {
            shared->length = std::stoll(value);
          } catch (const std::exception &) {
          }
        } else if (key == "content-range") {
          const std::size_t slash = value.rfind('/');
          if (slash != std::string::npos) {
            const std::string part = value.substr(slash + 1);
            if (part != "*") {
              try {
                shared->length = std::stoll(part);
              } catch (const std::exception &) {
              }
            }
          }
        }
      }
    }
    if (blank) {
      shared->headers_ready = true;
    }
  }
  if (blank) {
    shared->cv.notify_all();
  }
  return total;
}

int progress_cb(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  auto *shared = static_cast<SharedStream *>(user);
  return shared->cancelled ? 1 : 0;
}

} // namespace

bool isHttpUrl(const std::string &url) {
  return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

bool parseUrl(const std::string &url, Url &out) {
  Url parsed;
  std::string rest;
  if (url.rfind("https://", 0) == 0) {
    parsed.https = true;
    rest = url.substr(8);
  } else if (url.rfind("http://", 0) == 0) {
    parsed.https = false;
    rest = url.substr(7);
  } else {
    return false;
  }

  const std::size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  parsed.path = slash == std::string::npos ? "/" : rest.substr(slash);

  const std::size_t at = authority.rfind('@');
  if (at != std::string::npos) {
    const std::string credentials = authority.substr(0, at);
    authority = authority.substr(at + 1);
    const std::size_t colon = credentials.find(':');
    if (colon == std::string::npos) {
      parsed.user = credentials;
    } else {
      parsed.user = credentials.substr(0, colon);
      parsed.pass = credentials.substr(colon + 1);
    }
  }

  if (authority.empty()) {
    return false;
  }
  if (!authority.empty() && authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string::npos) {
      return false;
    }
    parsed.host = authority.substr(0, close + 1);
    if (close + 1 < authority.size() && authority[close + 1] == ':') {
      parsed.port = authority.substr(close + 2);
    }
  } else {
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
      parsed.host = authority.substr(0, colon);
      parsed.port = authority.substr(colon + 1);
    } else {
      parsed.host = authority;
    }
  }
  if (parsed.host.empty()) {
    return false;
  }
  out = std::move(parsed);
  return true;
}

std::string encodePathSegment(const std::string &segment) {
  std::string out;
  out.reserve(segment.size());
  for (const unsigned char c : segment) {
    if (isUnreserved(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out += hexByte(c);
    }
  }
  return out;
}

std::string buildUrl(const Url &base, const std::string &encoded_path) {
  std::string path = encoded_path.empty() ? std::string("/") : encoded_path;
  if (path.front() != '/') {
    path.insert(path.begin(), '/');
  }
  return origin(base) + path;
}

std::string Response::header(const std::string &key) const {
  const auto it = headers.find(lower(key));
  return it == headers.end() ? std::string() : it->second;
}

Response Client::request(const std::string &method, const std::string &path,
                         const std::vector<std::string> &extra_headers,
                         const std::string &body) {
  static std::once_flag curl_once;
  std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  Response out;
  CURL *handle = curl_easy_init();
  if (handle == nullptr) {
    return out;
  }
  std::string path_value = path.empty() ? std::string("/") : path;
  if (path_value.front() != '/') {
    path_value.insert(path_value.begin(), '/');
  }
  const std::string url = origin(base_) + path_value;

  struct curl_slist *headers = nullptr;
  for (const std::string &header : extra_headers) {
    headers = curl_slist_append(headers, header.c_str());
  }
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method.c_str());
  if (!body.empty() || method == "POST" || method == "PUT") {
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));
  }
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt(handle, CURLOPT_HEADERDATA, &out.headers);
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_cb);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &out.body);
  setCommonOptions(handle, base_);

  const CURLcode code = curl_easy_perform(handle);
  if (code == CURLE_OK) {
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    out.status = status;
  }
  if (headers != nullptr) {
    curl_slist_free_all(headers);
  }
  curl_easy_cleanup(handle);
  return out;
}

Stream openStream(const Url &base, const std::string &path,
                  const std::vector<std::string> &extra_headers,
                  std::int64_t header_timeout_ms) {
  static std::once_flag curl_once;
  std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  auto shared = std::make_shared<SharedStream>();
  std::string path_value = path.empty() ? std::string("/") : path;
  if (path_value.front() != '/') {
    path_value.insert(path_value.begin(), '/');
  }
  const std::string url = origin(base) + path_value;
  const Url base_copy = base;
  const std::vector<std::string> headers_copy = extra_headers;

  std::thread producer([shared, url, base_copy, headers_copy]() {
    CURL *handle = curl_easy_init();
    if (handle == nullptr) {
      std::lock_guard<std::mutex> lock(shared->mutex);
      shared->failed = true;
      shared->error = static_cast<int>(CURLE_FAILED_INIT);
      shared->finished = true;
      shared->headers_ready = true;
      shared->cv.notify_all();
      return;
    }
    struct curl_slist *headers = nullptr;
    for (const std::string &header : headers_copy) {
      headers = curl_slist_append(headers, header.c_str());
    }
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, stream_header_cb);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, shared.get());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, stream_appendix_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, shared.get());
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, shared.get());
    setCommonOptions(handle, base_copy);

    const CURLcode code = curl_easy_perform(handle);
    {
      std::lock_guard<std::mutex> lock(shared->mutex);
      shared->finished = true;
      shared->headers_ready = true;
      if (code != CURLE_OK) {
        shared->failed = true;
        shared->error = static_cast<int>(code);
      }
      if (shared->status == 0 && code == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        shared->status = status;
      }
    }
    shared->cv.notify_all();

    if (headers != nullptr) {
      curl_slist_free_all(headers);
    }
    curl_easy_cleanup(handle);
  });
  producer.detach();

  {
    std::unique_lock<std::mutex> lock(shared->mutex);
    const auto ready = [&shared] { return shared->headers_ready; };
    if (header_timeout_ms > 0) {
      if (!shared->cv.wait_for(
              lock, std::chrono::milliseconds(header_timeout_ms), ready)) {
        // 响应头迟迟不来：让生产者尽快收手，并把这次失败标成超时。
        shared->timed_out = true;
        shared->failed = true;
        shared->cancelled = true;
        lock.unlock();
        shared->cv.notify_all();
        lock.lock();
      }
    } else {
      shared->cv.wait(lock, ready);
    }
  }

  Stream stream;
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    stream.status = static_cast<int>(shared->status);
    stream.length = shared->length;
    stream.headers = shared->headers;
    stream.error = shared->error;
    stream.timed_out = shared->timed_out;
  }
  stream.read = [shared](char *buf, std::size_t n) -> long long {
    if (n == 0) {
      return 0;
    }
    std::unique_lock<std::mutex> lock(shared->mutex);
    shared->cv.wait(lock, [&shared] {
      return !shared->chunks.empty() || shared->finished || shared->cancelled;
    });
    if (shared->cancelled) {
      return -1;
    }
    if (shared->chunks.empty()) {
      return shared->failed ? -1 : 0;
    }
    std::string chunk = std::move(shared->chunks.front());
    shared->chunks.pop_front();
    const std::size_t take = std::min<std::size_t>(n, chunk.size());
    std::memcpy(buf, chunk.data(), take);
    if (take < chunk.size()) {
      shared->chunks.emplace_front(chunk.substr(take));
    }
    return static_cast<long long>(take);
  };
  stream.close = [shared] { shared->cancel(); };
  return stream;
}

} // namespace http_client