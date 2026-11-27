#include "http_client.h"

#include "http_stream_buffer.h"

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
//
// 正文不在这里排队，而是进 StreamBuffer：它有字节水位，写满就挡住生产者，
// 从而把背压一路传回上游 socket。这里只保留「响应头 + 终态」这点小状态。
struct SharedStream {
  std::mutex mutex;
  std::condition_variable cv;
  std::shared_ptr<StreamBuffer> buffer;
  std::atomic<bool> cancelled{false};
  bool headers_ready = false;
  long status = 0;
  long long length = -1;
  std::map<std::string, std::string> headers;
  int error = 0;
  bool timed_out = false;
  // 是否跟随 3xx：中间跳转的响应头不能当作最终响应头。
  bool follow_redirects = false;

  void cancel() {
    cancelled.store(true);
    if (buffer) {
      buffer->cancel();
    }
    cv.notify_all();
  }
};

std::size_t stream_appendix_cb(char *buffer, std::size_t size,
                               std::size_t count, void *user) {
  auto *shared = static_cast<SharedStream *>(user);
  const std::size_t total = size * count;
  // 这里可能阻塞：水位满了要等消费者腾地方，这正是背压的落点。
  // 绝不能持有 shared->mutex 再等，否则 cancel() 拿不到锁会死锁。
  if (!shared->buffer->push(buffer, total)) {
    return 0; // 已取消：让 curl 中止上游传输。
  }
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
      // 跟随重定向时，3xx 只是中间跳转：把它当最终响应头会让调用方提前
      // 拿到 302 而误判失败。带 Location 的 3xx 继续等下一跳。
      const bool interim = shared->follow_redirects && shared->status >= 300 &&
                           shared->status < 400 && shared->status != 304 &&
                           shared->headers.find("location") !=
                               shared->headers.end();
      if (!interim) {
        shared->headers_ready = true;
      }
    }
  }
  if (blank) {
    shared->cv.notify_all();
  }
  return total;
}

int progress_cb(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  auto *shared = static_cast<SharedStream *>(user);
  return shared->cancelled.load() ? 1 : 0;
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
                  std::int64_t header_timeout_ms, int max_redirects,
                  std::size_t buffer_bytes) {
  static std::once_flag curl_once;
  std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  auto shared = std::make_shared<SharedStream>();
  shared->follow_redirects = max_redirects > 0;
  // 0 表示用默认水位（1 MiB）。水位是「不缓存整段」的唯一保障。
  constexpr std::size_t kDefaultBufferBytes = 1024 * 1024;
  shared->buffer = std::make_shared<StreamBuffer>(
      buffer_bytes == 0 ? kDefaultBufferBytes : buffer_bytes);
  std::string path_value = path.empty() ? std::string("/") : path;
  if (path_value.front() != '/') {
    path_value.insert(path_value.begin(), '/');
  }
  const std::string url = origin(base) + path_value;
  const Url base_copy = base;
  const std::vector<std::string> headers_copy = extra_headers;

  std::thread producer([shared, url, base_copy, headers_copy,
                        max_redirects]() {
    CURL *handle = curl_easy_init();
    if (handle == nullptr) {
      {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->error = static_cast<int>(CURLE_FAILED_INIT);
        shared->headers_ready = true;
      }
      shared->buffer->finish(/*failed=*/true);
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
    if (max_redirects > 0) {
      curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(handle, CURLOPT_MAXREDIRS,
                       static_cast<long>(max_redirects));
#if LIBCURL_VERSION_NUM >= 0x075500
      curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
      curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS,
                       CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    }

    const CURLcode code = curl_easy_perform(handle);
    {
      std::lock_guard<std::mutex> lock(shared->mutex);
      shared->headers_ready = true;
      if (code != CURLE_OK) {
        shared->error = static_cast<int>(code);
      }
      if (shared->status == 0 && code == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        shared->status = status;
      }
    }
    // 用 CURLE_WRITE_ERROR 单独判断：那是我们主动挡住生产者的结果，不是故障。
    const bool transport_failed =
        code != CURLE_OK && code != CURLE_WRITE_ERROR &&
        !shared->cancelled.load();
    shared->buffer->finish(transport_failed);
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
        lock.unlock();
        shared->cancel();
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
    // 语义：>0 字节数，0 正常结束，-1 取消或上游传输出错。
    // 三者必须区分 —— 把错误当 EOF 会让客户端把截断的音频当完整文件。
    return shared->buffer->pop(buf, n);
  };
  stream.close = [shared] { shared->cancel(); };
  return stream;
}

} // namespace http_client