#include "stream_http.h"

#include "api/subsonic/envelope.h"
#include "http_util.h"
#include "json_util.h"
#include "media_library/media_runtime.h"
#include "stream_plan.h"
#include "stream_scheduler.h"

#include <json/json.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace stream_http {
namespace {

using media_library::ByteStream;
using media_library::Error;
using media_library::ErrorCode;
using media_library::PlaybackService;
using media_library::PlaybackSession;
using media_library::Result;
using media_library::StreamInfo;

// 每次推送读一块的上限。与 ByteStream 内部的 kReadChunk 对齐。
constexpr std::size_t kPushChunk = 64 * 1024;
// 等 Drogon 把 ResponseStream 交回来、以及等慢客户端的默认上限。
constexpr int kDefaultSendTimeoutMs = 30000;
// 由 musicd 用配置覆盖；保持 trivially copyable，非 atomic 也不需要。
std::atomic<int> g_send_timeout_ms{kDefaultSendTimeoutMs};

// 唯一一份 ErrorCode -> HTTP 状态码映射；门面不要再各自抄一遍。
int status_for(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::InvalidRequest:
    return 400;
  case ErrorCode::AuthRequired:
    return 401;
  case ErrorCode::PermissionDenied:
    return 403;
  case ErrorCode::NotFound:
    return 404;
  case ErrorCode::Conflict:
    return 409;
  case ErrorCode::PlaybackExpired:
    return 410;
  case ErrorCode::RateLimited:
    return 429;
  case ErrorCode::Unsupported:
    return 501;
  case ErrorCode::ProviderUnavailable:
    return 503;
  case ErrorCode::Timeout:
    return 504;
  case ErrorCode::Cancelled:
    return 499;
  case ErrorCode::Internal:
    return 500;
  }
  return 500;
}

Json::Value error_json(const Error &error) {
  Json::Value detail;
  detail["code"] = std::string(media_library::error_code_name(error.code));
  detail["message"] = error.message;
  if (!error.library_id.empty()) {
    detail["library_id"] = error.library_id;
  }
  if (!error.capability.empty()) {
    detail["capability"] = error.capability;
  }
  if (error.restriction.has_value()) {
    detail["restriction"] =
        std::string(media_library::restriction_code_name(*error.restriction));
  }
  detail["retryable"] = error.retryable;
  if (error.retry_after.has_value()) {
    detail["retry_after"] = static_cast<Json::Int64>(*error.retry_after);
  }
  // details 与 internal_cause 是诊断字段，绝不出现在响应里。
  Json::Value body;
  body["error"] = std::move(detail);
  return body;
}

std::shared_ptr<PlaybackService> playback() {
  return media_library::playback_service_ptr();
}

// Subsonic：错误一律 HTTP 200（规范如此），错误只在信封里。
ResponsePtr subsonic_error(const Error &error) {
  const subsonic::Code code = subsonic::code_of(error.code);
  std::string message = error.message;
  if (code == subsonic::Code::Ok) {
    // 没有对应码（超时/上游不可用/内部错误）：把内部错误名写进 message，
    // 否则客户端只能看到一个 code=0 的哑巴错误。
    const std::string name = subsonic::error_name(error.code);
    message = message.empty() ? name : message + " (" + name + ")";
  }
  return subsonic::failure_response(code, message);
}

// Jellyfin：ProblemDetails，状态码用真实 HTTP 状态。
ResponsePtr jellyfin_error(const Error &error) {
  const int status = status_for(error.code);
  Json::Value problem;
  problem["type"] = "about:blank";
  problem["title"] = std::string(media_library::error_code_name(error.code));
  problem["status"] = status;
  problem["detail"] = error.message;
  if (!error.library_id.empty()) {
    problem["instance"] = "library://" + error.library_id;
  }
  return http_util::jsonResponse(status, json_util::dump(problem));
}

ResponsePtr rangeNotSatisfiable(std::int64_t size) {
  ResponsePtr res = http_server::textResponse(416, "range not satisfiable");
  if (size >= 0) {
    res->addHeader("Content-Range", "bytes */" + std::to_string(size));
  }
  return res;
}

// ---------------------------------------------------------------------------
// 推送式异步流的移交点
//
// Drogon 的 newAsyncStreamResponse 只会在连接 IO 线程上把 ResponseStream
// 交给我们的内层回调（HttpServer.cc:1000-1007）。而泵字节必须在 worker 线程
// 做，否则就会阻塞 IO 线程 —— 那正是 newStreamResponse 的老问题。
//
// 所以内层回调只做一件事：把指针放进这里并唤醒 worker。
// ---------------------------------------------------------------------------
struct StreamHandoff {
  std::mutex mutex;
  std::condition_variable cv;
  drogon::ResponseStreamPtr stream; // 由 IO 线程写入，worker 取走
  bool ready = false;
  // 下游背压：连接发送缓冲超过高水位时置真，worker 暂停读取。
  //
  // trantor 没有「发送缓冲已排空」的回报（writeCompleteCallback 在 1.5.29
  // 里根本没有被调用），所以高水位回调只能把标志置真、置不了假。worker
  // 用 bytesSent() 的增长作为「socket 还在写」的代理：只要它涨了就说明
  // 客户端在收，解除阻塞；一直不涨、超过 send_timeout_ms 就放弃这条连接。
  std::atomic<bool> client_blocked{false};
  // 由连接 IO 线程写入的 bytesSent() 采样（跨线程读 trantor 成员是数据竞争，
  // 所以只在 IO 线程上读、写进这个 atomic）。
  std::atomic<std::size_t> sent_bytes{0};
  // 触发高水位那一刻的 bytesSent()。worker 只有看到发送字节数超过这个
  // 快照才算「客户端确实在收」，否则一直等或超时放弃。
  std::atomic<std::size_t> blocked_at{0};
  std::condition_variable space_cv;
  // 采样定时器与连接：都只在 IO 线程上写；worker 只读 timer 用于注销。
  std::weak_ptr<trantor::TcpConnection> conn;
  std::atomic<trantor::TimerId> sampler{0};

  void deliver(drogon::ResponseStreamPtr value) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stream = std::move(value);
      ready = true;
    }
    cv.notify_all();
  }

  // 等 ResponseStream 到位。超时返回空。
  drogon::ResponseStreamPtr take(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                     [this] { return ready; })) {
      return nullptr;
    }
    return std::move(stream);
  }
};

// 高水位标记：让 worker 在发送缓冲堆起来时停手。
//
// 只有「超过水位」的回调，没有「降回水位以下」的回调，所以再补一个 50ms
// 的 bytesSent() 采样定时器：worker 一旦看到发送字节数在涨，就认为客户端
// 在收，自行解除阻塞。两个回调都必须在连接 IO 线程上装/跑。
void install_downstream_backpressure(const std::shared_ptr<StreamHandoff> &handoff,
                                     const RequestPtr &req) {
  if (!req) {
    return;
  }
  std::weak_ptr<trantor::TcpConnection> weak = req->getConnectionPtr();
  const auto conn = weak.lock();
  if (!conn) {
    // 拿不到连接（代理层/已断开）：跳过下游背压，只靠 send() 返回值与
    // 上游水位兜底。
    return;
  }
  handoff->conn = weak;
  trantor::EventLoop *loop = conn->getLoop();
  loop->runInLoop([handoff, weak]() {
    const auto conn = weak.lock();
    if (!conn) {
      return;
    }
    handoff->sent_bytes.store(conn->bytesSent());
    conn->setHighWaterMarkCallback(
        [handoff, weak](const trantor::TcpConnectionPtr &, std::size_t) {
          // 记下「堆起来那一刻」的发送量；worker 看到它继续涨就解除阻塞。
          if (const auto conn = weak.lock()) {
            handoff->blocked_at.store(conn->bytesSent());
          }
          handoff->client_blocked.store(true);
        },
        256 * 1024);
    const trantor::TimerId id =
        conn->getLoop()->runEvery(0.05, [handoff, weak]() {
          const auto conn = weak.lock();
          if (!conn) {
            return;
          }
          handoff->sent_bytes.store(conn->bytesSent());
          // 只是把 worker 从等待里捅醒，让它自己用 sent_bytes 判断是否解除阻塞。
          handoff->space_cv.notify_all();
        });
    handoff->sampler.store(id);
  });
}

// 注销采样定时器，避免连接活着时每 50ms 白跑一次。
//
// 注销动作要排在安装动作之后（都在 IO loop 上串行执行），所以不管 worker
// 多早退出，都不会漏掉定时器。
void remove_downstream_backpressure(const std::shared_ptr<StreamHandoff> &handoff) {
  const auto conn = handoff->conn.lock();
  if (!conn) {
    return;
  }
  std::weak_ptr<trantor::TcpConnection> weak = handoff->conn;
  conn->getLoop()->runInLoop([handoff, weak]() {
    const trantor::TimerId id = handoff->sampler.exchange(0);
    if (id == 0) {
      return;
    }
    if (const auto conn = weak.lock()) {
      conn->getLoop()->invalidateTimer(id);
      // 连接是 keep-alive 的，下一条请求会重新安装自己的回调；不清掉会
      // 让这个 handoff（含整条流）被连接一直攥住。
      conn->setHighWaterMarkCallback(nullptr, 0);
    }
  });
}

// 等下游腾出空间。返回 false 表示超时（调用方应 cancel + close）。
//
// 判据：触发高水位之后 bytesSent() 又涨了 —— 说明 socket 确实在往客户端
// 写，缓冲在变短。只看「超过水位」那一刻的快照，不会因为水位标志一直为真
// 而永久卡住。
bool await_downstream_space(const std::shared_ptr<StreamHandoff> &handoff,
                            int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  std::unique_lock<std::mutex> lock(handoff->mutex);
  while (handoff->client_blocked.load()) {
    const std::size_t sent = handoff->sent_bytes.load();
    if (sent > handoff->blocked_at.load()) {
      handoff->client_blocked.store(false);
      return true;
    }
    if (handoff->space_cv.wait_until(lock, deadline) ==
        std::cv_status::timeout) {
      return false;
    }
  }
  return true;
}

// 一个「已准备好、但还没交给 Drogon」的响应，以及交给之后要做的推送。
struct Prepared {
  ResponsePtr response;
  // 非空表示：callback(response) 之后，worker 还要继续泵字节。
  std::function<void()> pump;
};

// 把流按 Range 语义输出。返回要立即交给 Drogon 的响应 + 后续推送动作。
Prepared prepare(const RequestPtr &req, std::shared_ptr<ByteStream> stream,
                 const std::string &library_id, bool head_only, Protocol protocol,
                 int send_timeout_ms) {
  if (stream == nullptr) {
    return {error_response_for(
                protocol,
                Error::make(ErrorCode::Internal, "audio stream is null")),
            {}};
  }

  StreamInfo info = stream->info();
  // 远端流惰性连接：长度未知时先探测。这一步会阻塞，所以必须已经在
  // worker 线程上（serve_*_async 保证了这一点）。
  if (info.size_bytes < 0) {
    const media_library::Status prepared = stream->prepare();
    if (!prepared.ok()) {
      return {error_response_for(protocol, prepared.error()), {}};
    }
    info = stream->info();
  }

  const std::string range = req ? req->getHeader("Range") : std::string();
  const RangePlan plan =
      plan_range(range, info.size_bytes, info.seekable, head_only);

  if (plan.mode == RangePlan::Mode::Unsatisfiable) {
    return {rangeNotSatisfiable(plan.total), {}};
  }

  const std::string content_type =
      info.mime_type.empty() ? std::string("application/octet-stream")
                             : info.mime_type;

  // HEAD：只回头，不读字节。
  if (head_only) {
    ResponsePtr res = drogon::HttpResponse::newHttpResponse();
    res->setStatusCode(drogon::k200OK);
    res->setContentTypeString(content_type);
    if (plan.length >= 0) {
      res->addHeader("Content-Length", std::to_string(plan.length));
    }
    res->addHeader("Accept-Ranges", info.seekable ? "bytes" : "none");
    if (plan.total >= 0) {
      // HEAD 不适用 206/416，但真实总长照样告知，客户端可据此规划 Range。
      res->addHeader("Content-Range",
                     "bytes */" + std::to_string(plan.total));
    }
    if (!library_id.empty()) {
      res->addHeader("X-Musicd-Origin", library_id);
    }
    return {res, {}};
  }

  // 需要跳转的 Range：在 worker 上 seek（远端会带 Range 重开连接）。
  if (plan.start > 0) {
    const media_library::Status seeked = stream->seek(plan.start);
    if (!seeked.ok()) {
      return {error_response_for(protocol, seeked.error()), {}};
    }
  }

  // 本地文件：交给 Drogon 的零拷贝文件响应，Range/206/416 都由框架处理。
  if (const auto path = stream->local_path(); path.has_value()) {
    const std::size_t offset = static_cast<std::size_t>(plan.start);
    const std::size_t length =
        plan.length >= 0 ? static_cast<std::size_t>(plan.length) : 0;
    ResponsePtr res = drogon::HttpResponse::newFileResponse(
        *path, offset, length, /*setContentRange=*/plan.mode ==
                                   RangePlan::Mode::Partial,
        content_type);
    res->setStatusCode(plan.mode == RangePlan::Mode::Partial ? drogon::k206PartialContent
                                                            : drogon::k200OK);
    res->addHeader("Accept-Ranges", "bytes");
    if (!library_id.empty()) {
      res->addHeader("X-Musicd-Origin", library_id);
    }
    return {res, {}};
  }

  // 远程流：chunked 推送。绝不能再设 Content-Length —— ResponseStream::send
  // 自己会做 chunk 分帧，同时声明长度会让头与正文矛盾。
  auto handoff = std::make_shared<StreamHandoff>();
  ResponsePtr res = drogon::HttpResponse::newAsyncStreamResponse(
      [handoff](drogon::ResponseStreamPtr value) {
        handoff->deliver(std::move(value));
      },
      /*disableKickoffTimeout=*/true);
  res->setStatusCode(plan.mode == RangePlan::Mode::Partial
                         ? drogon::k206PartialContent
                         : drogon::k200OK);
  res->setContentTypeString(content_type);
  res->addHeader("Accept-Ranges", info.seekable ? "bytes" : "none");
  if (plan.send_content_range && plan.end >= 0) {
    res->addHeader("Content-Range",
                   "bytes " + std::to_string(plan.start) + "-" +
                       std::to_string(plan.end) + "/" +
                       std::to_string(plan.total));
  }
  if (!library_id.empty()) {
    res->addHeader("X-Musicd-Origin", library_id);
  }

  // 下游背压：必须在响应真正开始发送前把高水位回调装好，否则慢客户端
  // 阶段没有信号可用。
  install_downstream_backpressure(handoff, req);

  std::shared_ptr<ByteStream> owned = std::move(stream);
  std::function<void()> pump = [owned, plan, handoff, send_timeout_ms]() {
    // 无论从哪个分支退出，都要把采样定时器摘掉。
    struct SamplerGuard {
      std::shared_ptr<StreamHandoff> handoff;
      ~SamplerGuard() { remove_downstream_backpressure(handoff); }
    } guard{handoff};

    drogon::ResponseStreamPtr out = handoff->take(send_timeout_ms);
    if (out == nullptr) {
      // 等不到 ResponseStream：响应头已发出，只能中止上游并断连。
      owned->cancel();
      return;
    }

    std::int64_t remaining = plan.length;
    for (;;) {
      if (remaining == 0) {
        break;
      }
      // 慢客户端：发送缓冲到高水位时暂停读，避免把整个文件堆进内存。
      // 超时就直接放弃这条连接，别让 worker 永久挂着。
      if (!await_downstream_space(handoff, send_timeout_ms)) {
        owned->cancel();
        out->close();
        return;
      }

      std::size_t want = kPushChunk;
      if (remaining > 0) {
        want = static_cast<std::size_t>(
            std::min<std::int64_t>(remaining, static_cast<std::int64_t>(want)));
      }
      Result<std::vector<std::byte>> chunk = owned->read(want);
      if (!chunk.ok()) {
        // 首字节之后失败：不发错误体（头已经发出去了），中止上游并断连。
        owned->cancel();
        out->close();
        return;
      }
      if (chunk.value().empty()) {
        break; // 正常 EOF
      }
      std::string data(reinterpret_cast<const char *>(chunk.value().data()),
                       chunk.value().size());
      if (!out->send(data)) {
        // 客户端断开：立刻停止拉上游，别白白下载。
        owned->cancel();
        return;
      }
      if (remaining > 0) {
        remaining -= static_cast<std::int64_t>(data.size());
      }
    }
    out->close();
  };
  return {res, std::move(pump)};
}

// 在池上执行「prepare + 回调 + 推送」。这是所有字节出口的唯一落点。
void serve_on_pool(bool image, RequestPtr req, Protocol protocol,
                   std::function<Prepared()> work, AsyncCallback callback) {
  auto task = [req = std::move(req), work = std::move(work),
               callback = std::move(callback)]() mutable {
    Prepared prepared = work();
    ResponsePtr response = std::move(prepared.response);
    std::function<void()> pump = std::move(prepared.pump);
    callback(response);
    if (pump) {
      pump();
    }
  };

  const bool queued = image ? Scheduler::instance().submit_image(std::move(task))
                            : Scheduler::instance().submit_audio(std::move(task));
  if (!queued) {
    // 队列满：同步回 503，不占 worker。Retry-After 让客户端稍后重试。
    Error error = Error::make(ErrorCode::ProviderUnavailable,
                              "stream workers are busy");
    error.retryable = true;
    error.retry_after = 1;
    callback(error_response_for(protocol, error));
  }
}

int send_timeout_ms() {
  // 由 musicd 在启动时用 config 注入；未配置时用默认值。
  return g_send_timeout_ms.load();
}

} // namespace

int status_of(ErrorCode code) noexcept { return status_for(code); }

void configure(const StreamHttpOptions &options) {
  if (options.send_timeout_ms > 0) {
    g_send_timeout_ms.store(options.send_timeout_ms);
  }
}

ResponsePtr error_response(const Error &error) {
  ResponsePtr res = http_util::jsonResponse(status_for(error.code),
                                            json_util::dump(error_json(error)));
  if (error.retry_after.has_value()) {
    res->addHeader("Retry-After", std::to_string(*error.retry_after));
  }
  return res;
}

ResponsePtr error_response_for(Protocol protocol, const Error &error) {
  ResponsePtr res;
  switch (protocol) {
  case Protocol::Subsonic:
    // Subsonic 规范里错误也用 HTTP 200，错误只在信封里。
    res = subsonic_error(error);
    break;
  case Protocol::Jellyfin:
    res = jellyfin_error(error);
    break;
  case Protocol::Native:
  default:
    res = error_response(error);
    break;
  }
  // 响应头不分协议：Retry-After 三种门面都照发（Subsonic 客户端也认这个头）。
  if (res != nullptr && error.retry_after.has_value() &&
      res->getHeader("retry-after").empty()) {
    res->addHeader("Retry-After", std::to_string(*error.retry_after));
  }
  return res;
}

void serve_session_async(const RequestPtr &req, const std::string &session_id,
                         bool head_only, AsyncCallback callback) {
  serve_on_pool(
      /*image=*/false, req, Protocol::Native,
      [req, session_id, head_only]() -> Prepared {
        const std::shared_ptr<PlaybackService> service = playback();
        if (service == nullptr) {
          return {error_response(Error::make(
                      ErrorCode::ProviderUnavailable,
                      "media runtime is not initialized")),
                  {}};
        }
        Result<PlaybackSession> session = service->session(session_id);
        if (!session.ok()) {
          return {error_response(session.error()), {}};
        }
        Result<std::unique_ptr<ByteStream>> opened =
            service->open_stream(session_id, media_library::CallContext{});
        if (!opened.ok()) {
          return {error_response(opened.error()), {}};
        }
        std::shared_ptr<ByteStream> stream(std::move(opened).value());
        return prepare(req, std::move(stream),
                       session.value().stream.origin.library_id, head_only,
                       Protocol::Native, send_timeout_ms());
      },
      std::move(callback));
}

void serve_track_async(const RequestPtr &req,
                       const media_library::TrackRef &track, bool head_only,
                       Protocol protocol, AsyncCallback callback) {
  // 本地库与远端库都在路由层选源：本地库的 locator 指向存储服务，
  // 探测失败时由 Router 换到同一链接里的其它库（见
  // document/STORAGE_PROXY_DECISIONS.md）。没有链接则退化为单源。
  serve_on_pool(
      /*image=*/false, req, protocol,
      [req, track, head_only, protocol]() -> Prepared {
        const std::shared_ptr<PlaybackService> service = playback();
        if (service == nullptr) {
          return {error_response_for(protocol, Error::make(
                                                    ErrorCode::ProviderUnavailable,
                                                    "media runtime is not "
                                                    "initialized")),
                  {}};
        }
        Result<media_library::OpenedStream> opened =
            service->open_stream_for(track, media_library::CallContext{});
        if (!opened.ok()) {
          return {error_response_for(protocol, opened.error()), {}};
        }
        media_library::OpenedStream value = std::move(opened).value();
        return prepare(req, std::move(value.stream), value.library_id,
                       head_only, protocol, send_timeout_ms());
      },
      std::move(callback));
}

void serve_artwork_async(const RequestPtr &req,
                         const media_library::ArtworkRef &artwork,
                         bool head_only, Protocol protocol,
                         AsyncCallback callback) {
  // 封面与音频同一条出口：统一交给 ArtworkService 路由选源，失败可回退。
  // 封面走图片池；缓存命中/本地文件在 prepare 里变成零拷贝文件响应。
  serve_on_pool(
      /*image=*/true, req, protocol,
      [req, artwork, head_only, protocol]() -> Prepared {
        const std::shared_ptr<media_library::ArtworkService> service =
            media_library::artwork_service_ptr();
        if (service == nullptr) {
          return {error_response_for(protocol,
                                     Error::make(ErrorCode::ProviderUnavailable,
                                                 "media runtime is not "
                                                 "initialized")),
                  {}};
        }
        const media_library::CallContext ctx;
        // 只解析一次：info 与字节流出自同一次路由，不再各跑一遍（Q12）。
        // HEAD 不需要字节流；其余情况连流一起建（远端流是惰性连接，不花代价）。
        Result<media_library::OpenedArtwork> opened =
            service->open(artwork, ctx, /*want_stream=*/!head_only);
        if (!opened.ok()) {
          return {error_response_for(protocol, opened.error()), {}};
        }
        media_library::OpenedArtwork value = std::move(opened).value();
        const media_library::ArtworkInfo &info = value.info;
        const std::string &etag = info.etag;
        if (!etag.empty() && req) {
          const std::string request_etag = req->getHeader("If-None-Match");
          if (!request_etag.empty() && request_etag == etag) {
            ResponsePtr not_modified = drogon::HttpResponse::newHttpResponse();
            not_modified->setStatusCode(drogon::k304NotModified);
            not_modified->addHeader("ETag", etag);
            not_modified->addHeader("Cache-Control", "public, max-age=3600");
            return {not_modified, {}};
          }
        }

        // HEAD 时没有流：用元信息自行拼一个只回头部的响应。
        if (head_only || value.stream == nullptr) {
          ResponsePtr res = drogon::HttpResponse::newHttpResponse();
          res->setStatusCode(drogon::k200OK);
          res->setContentTypeString(
              info.mime_type.empty() ? std::string("application/octet-stream")
                                     : info.mime_type);
          if (info.size_bytes >= 0) {
            res->addHeader("Content-Length", std::to_string(info.size_bytes));
            res->addHeader("Accept-Ranges", "bytes");
          } else {
            res->addHeader("Accept-Ranges", "none");
          }
          if (!etag.empty()) {
            res->addHeader("ETag", etag);
          }
          res->addHeader("Cache-Control", "public, max-age=3600");
          if (!info.library_id.empty()) {
            res->addHeader("X-Musicd-Origin", info.library_id);
          }
          return {res, {}};
        }

        std::shared_ptr<ByteStream> stream(std::move(value.stream));
        Prepared prepared =
            prepare(req, std::move(stream), info.library_id, head_only, protocol,
                    send_timeout_ms());
        if (!etag.empty() && prepared.response) {
          prepared.response->addHeader("ETag", etag);
        }
        if (prepared.response) {
          prepared.response->addHeader("Cache-Control", "public, max-age=3600");
        }
        return prepared;
      },
      std::move(callback));
}

} // namespace stream_http
