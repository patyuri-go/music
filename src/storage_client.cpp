#include "storage_client.h"

#include "http_util.h"

#include <curl/curl.h>
#include <taglib/aifffile.h>
#include <taglib/apefile.h>
#include <taglib/dsdifffile.h>
#include <taglib/dsffile.h>
#include <taglib/flacfile.h>
#include <taglib/mp4file.h>
#include <taglib/mpcfile.h>
#include <taglib/mpegfile.h>
#include <taglib/oggflacfile.h>
#include <taglib/opusfile.h>
#include <taglib/trueaudiofile.h>
#include <taglib/vorbisfile.h>
#include <taglib/wavfile.h>
#include <taglib/wavpackfile.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace storage_client {
namespace {

constexpr std::size_t kHeadBytes = 64 * 1024;
constexpr std::size_t kMaxProbeBytes = 1024 * 1024;

// ------------------------------------------------------------------ 线程池

class WorkerPool {
public:
  explicit WorkerPool(int count) {
    for (int i = 0; i < count; ++i) {
      workers_.emplace_back([this] { loop(); });
    }
  }

  ~WorkerPool() { stop(); }

  WorkerPool(const WorkerPool &) = delete;
  WorkerPool &operator=(const WorkerPool &) = delete;

  void submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        return;
      }
      tasks_.push(std::move(task));
    }
    cv_.notify_one();
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (std::thread &worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

private:
  void loop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty()) {
          if (stopping_) {
            return;
          }
          continue;
        }
        task = std::move(tasks_.front());
        tasks_.pop();
      }
      task();
    }
  }

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stopping_ = false;
};

// ------------------------------------------------------------------ 全局

Options g_options;
std::mutex g_init_mutex;
std::unique_ptr<WorkerPool> g_pool;
std::atomic<bool> g_ready{false};
std::atomic<int> g_active{0};
std::once_flag g_curl_once;

std::string trim(std::string text) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  text.erase(text.begin(),
             std::find_if(text.begin(), text.end(), not_space));
  text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(),
             text.end());
  return text;
}

std::string lower(std::string text) {
  for (char &ch : text) {
    ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  }
  return text;
}

http_server::ResponsePtr error_response(int code) {
  return http_server::textResponse(code, R"({"erro":true})",
                                   "application/json; charset=utf-8");
}

std::string api_url(const std::string &entity_id) {
  return g_options.scheme + "://" + g_options.host + ":" +
         std::to_string(g_options.port) + "/api/entities/" + entity_id +
         "/content";
}

// ------------------------------------------------------------------ HTTP

struct Headers {
  long status = 0;
  std::string content_type;
  std::string content_range;
  std::string etag;
  std::string accept_ranges;
  long long content_length = -1;
};

struct Sink {
  Headers headers;
  std::string body;
  std::size_t limit = kHeadBytes;
  bool overflow = false;
};

std::size_t header_cb(char *buffer, std::size_t size, std::size_t count,
                      void *user) {
  const std::size_t total = size * count;
  auto *sink = static_cast<Sink *>(user);
  std::string line(buffer, total);
  while (!line.empty() &&
         (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  if (line.empty()) {
    return total;
  }
  if (line.rfind("HTTP/", 0) == 0) {
    const std::size_t space = line.find(' ');
    if (space != std::string::npos) {
      try {
        sink->headers.status = std::stol(line.substr(space + 1));
      } catch (const std::exception &) {
      }
    }
    return total;
  }
  const std::size_t colon = line.find(':');
  if (colon == std::string::npos) {
    return total;
  }
  const std::string key = lower(trim(line.substr(0, colon)));
  const std::string value = trim(line.substr(colon + 1));
  if (key == "content-type") {
    sink->headers.content_type = value;
  } else if (key == "content-range") {
    sink->headers.content_range = value;
  } else if (key == "etag") {
    sink->headers.etag = value;
  } else if (key == "accept-ranges") {
    sink->headers.accept_ranges = value;
  } else if (key == "content-length") {
    try {
      sink->headers.content_length = std::stoll(value);
    } catch (const std::exception &) {
    }
  }
  return total;
}

std::size_t body_cb(char *buffer, std::size_t size, std::size_t count,
                    void *user) {
  const std::size_t total = size * count;
  auto *sink = static_cast<Sink *>(user);
  const std::size_t room =
      sink->body.size() < sink->limit ? sink->limit - sink->body.size() : 0;
  if (total > room) {
    sink->body.append(buffer, room);
    sink->overflow = true;
    return 0;  // abort: we only need the head.
  }
  sink->body.append(buffer, total);
  return total;
}

struct RequestHeaders {
  std::string range;
  std::string if_range;
  std::string if_none_match;
};

curl_slist *make_headers(const RequestHeaders &extra) {
  curl_slist *list = nullptr;
  list = curl_slist_append(list, "Accept: */*");
  if (!extra.range.empty()) {
    list = curl_slist_append(list, ("Range: " + extra.range).c_str());
  }
  if (!extra.if_range.empty()) {
    list = curl_slist_append(list, ("If-Range: " + extra.if_range).c_str());
  }
  if (!extra.if_none_match.empty()) {
    list = curl_slist_append(
        list, ("If-None-Match: " + extra.if_none_match).c_str());
  }
  if (!g_options.token.empty()) {
    list = curl_slist_append(
        list, ("Authorization: Bearer " + g_options.token).c_str());
  }
  return list;
}

struct Probe {
  CURLcode code = CURLE_FAILED_INIT;
  Sink sink;
};

Probe probe_once(const std::string &entity_id, const RequestHeaders &extra,
                 bool body_less, std::size_t limit) {
  Probe result;
  result.sink.limit = std::min(limit, kMaxProbeBytes);
  CURL *handle = curl_easy_init();
  if (handle == nullptr) {
    return result;
  }
  const std::string url = api_url(entity_id);
  curl_slist *headers = make_headers(extra);
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(g_options.timeout_ms));
  curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME,
                   static_cast<long>(std::max(1, g_options.timeout_ms / 1000)));
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt(handle, CURLOPT_HEADERDATA, &result.sink);
  if (body_less) {
    curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
  } else {
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &result.sink);
  }
  result.code = curl_easy_perform(handle);
  curl_slist_free_all(headers);
  curl_easy_cleanup(handle);
  return result;
}

// 仅在还没收到任何 HTTP 状态时重试一次传输错误。超时不重试：否则一次
// storage_timeout_ms 的故障会 doubled 成两倍等待，504 失去意义。
bool retryable(CURLcode code) {
  switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_GOT_NOTHING:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
      return true;
    default:
      return false;
  }
}

Probe probe(const std::string &entity_id, const RequestHeaders &extra,
            bool body_less, std::size_t limit) {
  Probe result = probe_once(entity_id, extra, body_less, limit);
  if (result.sink.headers.status == 0 && retryable(result.code)) {
    result = probe_once(entity_id, extra, body_less, limit);
  }
  return result;
}

long long total_from(const Sink &sink) {
  const std::string &range = sink.headers.content_range;
  if (!range.empty()) {
    const std::size_t slash = range.rfind('/');
    if (slash != std::string::npos) {
      const std::string part = range.substr(slash + 1);
      if (part != "*") {
        try {
          return std::stoll(part);
        } catch (const std::exception &) {
        }
      }
    }
  }
  if (sink.headers.content_length >= 0) {
    return sink.headers.content_length;
  }
  return -1;
}

// ------------------------------------------------------------------ 类型嗅探

std::string image_mime(const std::string &head) {
  const auto byte = [&head](std::size_t index) -> unsigned char {
    return index < head.size() ? static_cast<unsigned char>(head[index]) : 0;
  };
  if (head.size() >= 3 && byte(0) == 0xFF && byte(1) == 0xD8 &&
      byte(2) == 0xFF) {
    return "image/jpeg";
  }
  if (head.rfind("\x89PNG\r\n\x1a\n", 0) == 0) {
    return "image/png";
  }
  if (head.rfind("GIF87a", 0) == 0 || head.rfind("GIF89a", 0) == 0) {
    return "image/gif";
  }
  if (head.size() >= 12 && head.compare(0, 4, "RIFF") == 0 &&
      head.compare(8, 4, "WEBP") == 0) {
    return "image/webp";
  }
  if (head.size() >= 2 && head[0] == 'B' && head[1] == 'M') {
    return "image/bmp";
  }
  if (head.rfind("II*\0", 0) == 0 || head.rfind("MM\0*", 0) == 0) {
    return "image/tiff";
  }
  if (head.size() >= 12 && head.compare(4, 4, "ftyp") == 0 &&
      (head.compare(8, 4, "avif") == 0 || head.compare(8, 4, "avis") == 0)) {
    return "image/avif";
  }
  return std::string();
}

bool starts(const std::string &head, const char *magic, std::size_t n) {
  return head.size() >= n && std::memcmp(head.data(), magic, n) == 0;
}

// TagLib 只读到 64KB 头；识别不出时回退上游 Content-Type。
std::string audio_mime(const std::string &head) {
  if (head.empty()) {
    return std::string();
  }
  // TagLib 的对象需要 IOStream，这里直接按文件头构造一个最小的只读流。
  class MemoryStream : public TagLib::IOStream {
  public:
    explicit MemoryStream(std::string data) : data_(std::move(data)) {}
    TagLib::FileName name() const override { return "storage"; }
    TagLib::ByteVector readBlock(std::size_t length) override {
      const std::size_t room = data_.size() - position_;
      const std::size_t want = std::min(length, room);
      TagLib::ByteVector out;
      out.setData(data_.data() + position_,
                  static_cast<unsigned int>(want));
      position_ += want;
      return out;
    }
    void writeBlock(const TagLib::ByteVector &) override {}
    void insert(const TagLib::ByteVector &, TagLib::offset_t,
                std::size_t) override {}
    void removeBlock(TagLib::offset_t, std::size_t) override {}
    bool readOnly() const override { return true; }
    bool isOpen() const override { return true; }
    TagLib::offset_t length() override {
      return static_cast<TagLib::offset_t>(data_.size());
    }
    void truncate(TagLib::offset_t) override {}
    void seek(TagLib::offset_t offset, Position p) override {
      TagLib::offset_t base = 0;
      if (p == Current) {
        base = static_cast<TagLib::offset_t>(position_);
      } else if (p == End) {
        base = static_cast<TagLib::offset_t>(data_.size());
      }
      const TagLib::offset_t next = base + offset;
      if (next < 0) {
        position_ = 0;
      } else if (next > static_cast<TagLib::offset_t>(data_.size())) {
        position_ = data_.size();
      } else {
        position_ = static_cast<std::size_t>(next);
      }
    }
    TagLib::offset_t tell() const override {
      return static_cast<TagLib::offset_t>(position_);
    }

  private:
    std::string data_;
    std::size_t position_ = 0;
  };

  MemoryStream stream(head);
  const auto valid = [](TagLib::File &file) { return file.isValid(); };

  if (starts(head, "fLaC", 4)) {
    TagLib::FLAC::File file(&stream);
    if (valid(file)) return "audio/flac";
  } else if (starts(head, "ID3", 3) ||
             (static_cast<unsigned char>(head[0]) == 0xFF)) {
    stream.seek(0, TagLib::IOStream::Beginning);
    TagLib::MPEG::File file(&stream);
    if (valid(file)) return "audio/mpeg";
  } else if (starts(head, "OggS", 4)) {
    if (head.find("OpusHead") != std::string::npos) {
      stream.seek(0, TagLib::IOStream::Beginning);
      TagLib::Ogg::Opus::File file(&stream);
      if (valid(file)) return "audio/opus";
    }
    if (head.find("vorbis") != std::string::npos) {
      stream.seek(0, TagLib::IOStream::Beginning);
      TagLib::Ogg::Vorbis::File file(&stream);
      if (valid(file)) return "audio/ogg";
    }
    if (head.find("FLAC") != std::string::npos) {
      stream.seek(0, TagLib::IOStream::Beginning);
      TagLib::Ogg::FLAC::File file(&stream);
      if (valid(file)) return "audio/flac";
    }
  } else if (head.size() > 12 && head.compare(4, 4, "ftyp") == 0) {
    stream.seek(0, TagLib::IOStream::Beginning);
    TagLib::MP4::File file(&stream);
    if (valid(file)) return "audio/mp4";
  } else if (starts(head, "RIFF", 4) && head.size() > 12 &&
             head.compare(8, 4, "WAVE") == 0) {
    stream.seek(0, TagLib::IOStream::Beginning);
    TagLib::RIFF::WAV::File file(&stream);
    if (valid(file)) return "audio/wav";
  } else if (starts(head, "FORM", 4) && head.size() > 12 &&
             (head.compare(8, 4, "AIFF") == 0 ||
              head.compare(8, 4, "AIFC") == 0)) {
    stream.seek(0, TagLib::IOStream::Beginning);
    TagLib::RIFF::AIFF::File file(&stream);
    if (valid(file)) return "audio/aiff";
  } else if (starts(head, "MAC ", 4)) {
    stream.seek(0, TagLib::IOStream::Beginning);
    TagLib::APE::File file(&stream);
    if (valid(file)) return "audio/ape";
  } else if (starts(head, "wvpk", 4)) {
    stream.seek(0, TagLib::IOStream::Beginning);
    TagLib::WavPack::File file(&stream);
    if (valid(file)) return "audio/wavpack";
  } else if (starts(head, "TTA1", 4)) {
    stream.seek(0, TagLib::IOStream::Beginning);
    TagLib::TrueAudio::File file(&stream);
    if (valid(file)) return "audio/tta";
  } else if (starts(head, "MPCK", 4) || starts(head, "MP+", 3)) {
    stream.seek(0, TagLib::IOStream::Beginning);
    TagLib::MPC::File file(&stream);
    if (valid(file)) return "audio/musepack";
  } else if (starts(head, "DSD ", 4)) {
    stream.seek(0, TagLib::IOStream::Beginning);
    TagLib::DSF::File file(&stream);
    if (valid(file)) return "audio/dsf";
  } else if (starts(head, "FRM8", 4)) {
    stream.seek(0, TagLib::IOStream::Beginning);
    TagLib::DSDIFF::File file(&stream);
    if (valid(file)) return "audio/dff";
  }
  return std::string();
}

// ------------------------------------------------------------------ 流

struct BodyContext {
  drogon::ResponseStreamPtr stream;
  bool failed = false;
};

std::size_t stream_write_cb(char *buffer, std::size_t size, std::size_t count,
                            void *user) {
  auto *ctx = static_cast<BodyContext *>(user);
  const std::size_t total = size * count;
  if (!ctx->stream || !ctx->stream->send(std::string(buffer, total))) {
    ctx->failed = true;
    return 0;
  }
  return total;
}

void pump_body(const std::string &entity_id, const RequestHeaders &extra,
               drogon::ResponseStreamPtr stream) {
  CURL *handle = curl_easy_init();
  if (handle == nullptr) {
    stream->close();
    g_active.fetch_sub(1);
    return;
  }
  BodyContext ctx{std::move(stream)};
  curl_slist *headers = make_headers(extra);
  const std::string url = api_url(entity_id);
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(g_options.timeout_ms));
  curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME,
                   static_cast<long>(
                       std::max(1, g_options.timeout_ms / 1000)));
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, stream_write_cb);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &ctx);
  curl_easy_perform(handle);
  curl_slist_free_all(headers);
  curl_easy_cleanup(handle);
  ctx.stream->close();
  g_active.fetch_sub(1);
}

// ------------------------------------------------------------------ 主流程

struct ClientRange {
  bool present = false;
  bool satisfiable = true;
  bool partial = false;
  long long start = 0;
  long long end = -1;
  long long length = -1;
  std::string forward;
};

ClientRange client_range_of(const std::string &header, long long total) {
  ClientRange out;
  if (header.empty()) {
    if (total >= 0) {
      out.end = total - 1;
      out.length = total;
    }
    return out;
  }
  out.present = true;
  if (total < 0) {
    // 上游没有给出长度：不猜区间，原样转发。
    out.partial = true;
    out.forward = header;
    return out;
  }
  if (total == 0) {
    out.satisfiable = false;
    return out;
  }
  const auto [start, end] = http_util::parseRange(header, total);
  if (start < 0) {
    out.satisfiable = false;
    return out;
  }
  out.start = start;
  out.end = end;
  out.length = end - start + 1;
  out.partial = !(start == 0 && end == total - 1);
  if (out.partial) {
    out.forward = "bytes=" + std::to_string(out.start) + "-" +
                  std::to_string(out.end);
  }
  return out;
}

void finish_error(Callback &callback, int code,
                  const std::string &content_range = std::string()) {
  http_server::ResponsePtr response = error_response(code);
  if (!content_range.empty()) {
    response->addHeader("Content-Range", content_range);
  }
  callback(response);
}

void serve_worker(const http_server::RequestPtr &req, std::string entity_id,
                  bool image, bool head_only,
                  Callback callback) {
  (void)req;
  RequestHeaders headers;
  const std::string client_range = std::string(req->getHeader("range"));
  headers.if_range = std::string(req->getHeader("if-range"));
  headers.if_none_match = std::string(req->getHeader("if-none-match"));

  // 探测请求总是取文件头，避免为嗅探下载整个文件。
  RequestHeaders probe_headers;
  probe_headers.if_none_match = headers.if_none_match;
  probe_headers.range = "bytes=0-65535";

  Probe probed = probe(entity_id, probe_headers, head_only, kHeadBytes);
  if (probed.sink.headers.status == 0) {
    if (probed.code == CURLE_OPERATION_TIMEDOUT) {
      finish_error(callback, 504);
    } else {
      finish_error(callback, 502);
    }
    return;
  }

  const long status = probed.sink.headers.status;
  if (status == 304) {
    http_server::ResponsePtr response = http_server::textResponse(304, "");
    if (!probed.sink.headers.etag.empty()) {
      response->addHeader("ETag", probed.sink.headers.etag);
    }
    if (image) {
      response->addHeader("Cache-Control", "public, max-age=3600");
    }
    callback(response);
    return;
  }
  if (status == 416) {
    finish_error(callback, 416, probed.sink.headers.content_range);
    return;
  }
  if (status >= 400) {
    int mapped = static_cast<int>(status);
    if (status == 401 || status == 404 || status == 429 ||
        status == 400) {
      mapped = static_cast<int>(status);
    } else {
      mapped = 502;
    }
    http_server::ResponsePtr response = error_response(mapped);
    if (!probed.sink.headers.content_range.empty()) {
      response->addHeader("Content-Range",
                          probed.sink.headers.content_range);
    }
    callback(response);
    return;
  }

  const long long total = total_from(probed.sink);
  ClientRange range = client_range_of(client_range, total);
  if (range.present && !range.satisfiable) {
    finish_error(callback, 416,
                 total >= 0 ? "bytes */" + std::to_string(total)
                            : std::string());
    return;
  }

  std::string mime = image ? image_mime(probed.sink.body)
                           : audio_mime(probed.sink.body);
  if (mime.empty()) {
    mime = probed.sink.headers.content_type;
  }
  if (mime.empty()) {
    mime = "application/octet-stream";
  }

  const int out_status = range.partial ? 206 : 200;
  const long long length = total < 0 ? -1 : range.length;

  http_server::ResponsePtr response;
  if (head_only) {
    response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(
        static_cast<drogon::HttpStatusCode>(out_status));
    response->setContentTypeString(mime);
    if (length >= 0) {
      response->addHeader("Content-Length", std::to_string(length));
    }
  } else {
    RequestHeaders body_headers;
    body_headers.range = range.forward;
    body_headers.if_range = headers.if_range;
    body_headers.if_none_match = headers.if_none_match;
    const int active = g_active.fetch_add(1) + 1;
    if (active > g_options.max_concurrent) {
      g_active.fetch_sub(1);
      finish_error(callback, 503);
      return;
    }
    response = drogon::HttpResponse::newAsyncStreamResponse(
        [entity_id, body_headers](drogon::ResponseStreamPtr stream) {
          std::thread([entity_id, body_headers,
                       stream = std::move(stream)]() mutable {
            pump_body(entity_id, body_headers, std::move(stream));
          }).detach();
        });
    response->setStatusCode(
        static_cast<drogon::HttpStatusCode>(out_status));
    response->setContentTypeString(mime);
  }

  response->addHeader("Accept-Ranges", "bytes");
  if (range.partial && total >= 0) {
    response->addHeader("Content-Range",
                        "bytes " + std::to_string(range.start) + "-" +
                            std::to_string(range.end) + "/" +
                            std::to_string(total));
  }
  if (!probed.sink.headers.etag.empty()) {
    response->addHeader("ETag", probed.sink.headers.etag);
  }
  if (image) {
    response->addHeader("Cache-Control", "public, max-age=3600");
  }
  response->addHeader("X-Musicd-Origin", "local");
  callback(response);
}

} // namespace

// ------------------------------------------------------------------ 对外

void initialize(const Options &options) {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  if (g_ready.load()) {
    return;
  }
  g_options = options;
  std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  g_pool = std::make_unique<WorkerPool>(std::max(1, options.workers));
  g_ready.store(true);
}

void shutdown() {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  if (!g_ready.exchange(false)) {
    return;
  }
  g_pool.reset();
}

bool enabled() { return g_ready.load(); }

std::string content_url(const std::string &entity_id) {
  return api_url(trim(entity_id));
}

int timeout_ms() { return g_options.timeout_ms; }

bool valid_entity_id(const std::string &entity_id) {
  const std::string id = trim(entity_id);
  if (id.size() != 36) {
    return false;
  }
  for (std::size_t i = 0; i < id.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (id[i] != '-') {
        return false;
      }
    } else if (!std::isxdigit(static_cast<unsigned char>(id[i]))) {
      return false;
    }
  }
  return true;
}

void serve_content(const http_server::RequestPtr &req,
                   const std::string &entity_id, bool image, bool head_only,
                   Callback callback) {
  if (!g_ready.load()) {
    callback(error_response(503));
    return;
  }
  const std::string id = trim(entity_id);
  if (!valid_entity_id(id)) {
    callback(error_response(404));
    return;
  }
  g_pool->submit([req, id, image, head_only,
                  callback = std::move(callback)]() mutable {
    serve_worker(req, id, image, head_only, std::move(callback));
  });
}

} // namespace storage_client