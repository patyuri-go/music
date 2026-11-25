#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/drogon.h>
#include <drogon/utils/HttpConstraint.h>

#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// HTTP 服务端底座：Drogon 之上的极薄一层。
//
// 只提供三件事：
//   1. 统一的请求/响应智能指针别名；
//   2. 响应构造助手（避免每个 handler 手搓 HttpResponse）;
//   3. 路由注册：同步 handler 直接返回 ResponsePtr；异步 handler 收下 Drogon
//      的 callback，可以交给工作线程稍后再回。
//
// 绝大多数 handler 写成「同步返回 ResponsePtr」，Drogon 的回调形态由 on()
// 内部适配；只有需要等待上游网络（如存储服务）的路由才用 onAsync()。
namespace http_server {

using RequestPtr = drogon::HttpRequestPtr;
using ResponsePtr = drogon::HttpResponsePtr;

// 纯文本响应（code + body 的等价物）。
ResponsePtr textResponse(int code, std::string body,
                         std::string content_type = "text/plain");

// 请求 query 的参数表。保留「顺序」与「重复键」，
// 因为 Subsonic/Jellyfin 的语义依赖多值（Ids=1&Ids=2）与大小写不敏感。
struct QueryParam {
  std::string key;
  std::string value;
};

class Query {
public:
  explicit Query(const RequestPtr &req);

  const std::vector<QueryParam> &all() const { return params_; }

  // 大小写不敏感取首个值。
  std::optional<std::string> get(const std::string &key) const;
  std::string getOr(const std::string &key, const std::string &fallback) const;
  bool has(const std::string &key) const;
  // 同名多值，保持请求中的顺序。
  std::vector<std::string> list(const std::string &key) const;

private:
  std::vector<QueryParam> params_;
};

namespace detail {

template <typename T> struct handler_traits;

template <typename R, typename... A>
struct handler_traits<R (*)(const RequestPtr &, A...)> {
  using args = std::tuple<A...>;
};

template <typename R, typename... A>
struct handler_traits<R (*)(RequestPtr, A...)> {
  using args = std::tuple<A...>;
};

template <typename R, typename C, typename... A>
struct handler_traits<R (C::*)(const RequestPtr &, A...) const> {
  using args = std::tuple<A...>;
};

template <typename R, typename C, typename... A>
struct handler_traits<R (C::*)(RequestPtr, A...) const> {
  using args = std::tuple<A...>;
};

// 允许 handler 省略 request 形参（只收路径参数，甚至一个都不收）。
// 这类形态的 operator() 签名没有 request，单独列一组特化。
template <typename R, typename... A>
struct handler_traits<R (*)(A...)> {
  using args = std::tuple<A...>;
};

template <typename R, typename C, typename... A>
struct handler_traits<R (C::*)(A...) const> {
  using args = std::tuple<A...>;
};

// 异步 handler：第二个形参是 Drogon 的 callback，可以留到工作线程再调用。
// 这里只提取路径参数，callback 本身不计入 args。
template <typename R, typename... A>
struct handler_traits<R (*)(const RequestPtr &,
                            std::function<void(const ResponsePtr &)>, A...)> {
  using args = std::tuple<A...>;
};

template <typename R, typename... A>
struct handler_traits<R (*)(RequestPtr,
                            std::function<void(const ResponsePtr &)>, A...)> {
  using args = std::tuple<A...>;
};

template <typename R, typename C, typename... A>
struct handler_traits<R (C::*)(const RequestPtr &,
                              std::function<void(const ResponsePtr &)>,
                              A...) const> {
  using args = std::tuple<A...>;
};

template <typename R, typename C, typename... A>
struct handler_traits<R (C::*)(RequestPtr,
                              std::function<void(const ResponsePtr &)>,
                              A...) const> {
  using args = std::tuple<A...>;
};

// 省略 request、只收 callback + 路径参数。
template <typename R, typename... A>
struct handler_traits<R (*)(std::function<void(const ResponsePtr &)>, A...)> {
  using args = std::tuple<A...>;
};

template <typename R, typename C, typename... A>
struct handler_traits<R (C::*)(std::function<void(const ResponsePtr &)>,
                              A...) const> {
  using args = std::tuple<A...>;
};

template <typename F>
struct handler_traits : handler_traits<decltype(&F::operator())> {};

// request 形参存在就传，不存在就只传路径参数。
template <typename F, typename... A>
ResponsePtr invoke_handler(F &f, const RequestPtr &req, A &&...args) {
  if constexpr (std::is_invocable_v<F &, const RequestPtr &, A...>) {
    return f(req, std::forward<A>(args)...);
  } else {
    return f(std::forward<A>(args)...);
  }
}

template <typename F, typename Tuple, std::size_t... I>
void register_path(drogon::HttpMethod method, const std::string &pattern, F f,
                   std::index_sequence<I...>) {
  drogon::app().registerHandler(
      pattern,
      [f](const RequestPtr &req,
          std::function<void(const ResponsePtr &)> &&callback,
          std::decay_t<std::tuple_element_t<I, Tuple>>... args) {
        callback(invoke_handler(f, req, std::move(args)...));
      },
      {drogon::internal::HttpConstraint{method}});
}

template <typename F, typename Tuple, std::size_t... I>
void register_regex(drogon::HttpMethod method, const std::string &pattern, F f,
                    std::index_sequence<I...>) {
  drogon::app().registerHandlerViaRegex(
      pattern,
      [f](const RequestPtr &req,
          std::function<void(const ResponsePtr &)> &&callback,
          std::decay_t<std::tuple_element_t<I, Tuple>>... args) {
        callback(invoke_handler(f, req, std::move(args)...));
      },
      {drogon::internal::HttpConstraint{method}});
}

// 异步 handler：callback 原样交出去，由 handler 决定何时/在哪个线程调用。
template <typename F, typename... A>
void invoke_async_handler(F &f, const RequestPtr &req,
                          std::function<void(const ResponsePtr &)> &&callback,
                          A &&...args) {
  if constexpr (std::is_invocable_v<
                    F &, const RequestPtr &,
                    std::function<void(const ResponsePtr &)>, A...>) {
    f(req, std::move(callback), std::forward<A>(args)...);
  } else {
    f(std::move(callback), std::forward<A>(args)...);
  }
}

template <typename F, typename Tuple, std::size_t... I>
void register_async_path(drogon::HttpMethod method, const std::string &pattern,
                         F f, std::index_sequence<I...>) {
  drogon::app().registerHandler(
      pattern,
      [f](const RequestPtr &req,
          std::function<void(const ResponsePtr &)> &&callback,
          std::decay_t<std::tuple_element_t<I, Tuple>>... args) {
        invoke_async_handler(f, req, std::move(callback),
                             std::move(args)...);
      },
      {drogon::internal::HttpConstraint{method}});
}

template <typename F, typename Tuple, std::size_t... I>
void register_async_regex(drogon::HttpMethod method, const std::string &pattern,
                          F f, std::index_sequence<I...>) {
  drogon::app().registerHandlerViaRegex(
      pattern,
      [f](const RequestPtr &req,
          std::function<void(const ResponsePtr &)> &&callback,
          std::decay_t<std::tuple_element_t<I, Tuple>>... args) {
        invoke_async_handler(f, req, std::move(callback),
                             std::move(args)...);
      },
      {drogon::internal::HttpConstraint{method}});
}

} // namespace detail

// 注册一条 `{1}`/`{2}` 风格的路径路由。
// handler 形态：ResponsePtr(const RequestPtr&, Arg1, Arg2, ...)，
// 参数个数必须与 pattern 里的占位符个数一致（Drogon 会校验）。
template <typename F>
void on(drogon::HttpMethod method, const std::string &pattern, F handler) {
  using args = typename detail::handler_traits<std::decay_t<F>>::args;
  detail::register_path<F, args>(
      method, pattern, std::move(handler),
      std::make_index_sequence<std::tuple_size<args>::value>{});
}

// 同 on()，但 pattern 是正则（用于含斜杠的路径参数，如 /web/(.*)）。
template <typename F>
void onRegex(drogon::HttpMethod method, const std::string &pattern, F handler) {
  using args = typename detail::handler_traits<std::decay_t<F>>::args;
  detail::register_regex<F, args>(
      method, pattern, std::move(handler),
      std::make_index_sequence<std::tuple_size<args>::value>{});
}

// 同 on()，但 handler 形态是
//   void(const RequestPtr&, std::function<void(const ResponsePtr&)>, Arg1, ...)
// callback 可以从任意线程调用；路由本身不阻塞 Drogon 的 IO 线程。
template <typename F>
void onAsync(drogon::HttpMethod method, const std::string &pattern, F handler) {
  using args = typename detail::handler_traits<std::decay_t<F>>::args;
  detail::register_async_path<F, args>(
      method, pattern, std::move(handler),
      std::make_index_sequence<std::tuple_size<args>::value>{});
}

template <typename F>
void onRegexAsync(drogon::HttpMethod method, const std::string &pattern,
                  F handler) {
  using args = typename detail::handler_traits<std::decay_t<F>>::args;
  detail::register_async_regex<F, args>(
      method, pattern, std::move(handler),
      std::make_index_sequence<std::tuple_size<args>::value>{});
}

// ---------------------------------------------------------------------------
// 进程内单 app / 多端口
//
// Drogon 是进程级单例（drogon::app()），一个进程只能有一个 app 实例。
// 主服务 / Jellyfin / Subsonic 三个监听端口因此共用同一个 app：
// 每个端口在入站时由 pre-routing advice 给 path 加上自己的前缀，
// 路由表靠前缀隔离，互不串扰。
// ---------------------------------------------------------------------------

std::string portPrefix(int port);

// 用 onPort()/onPortRegex() 注册端口专属路由。
template <typename F>
void onPort(int port, drogon::HttpMethod method, const std::string &path,
            F handler) {
  on(method, portPrefix(port) + path, std::move(handler));
}

template <typename F>
void onPortRegex(int port, drogon::HttpMethod method, const std::string &pattern,
                 F handler) {
  onRegex(method, portPrefix(port) + pattern, std::move(handler));
}

template <typename F>
void onPortAsync(int port, drogon::HttpMethod method, const std::string &path,
                 F handler) {
  onAsync(method, portPrefix(port) + path, std::move(handler));
}

template <typename F>
void onPortRegexAsync(int port, drogon::HttpMethod method,
                      const std::string &pattern, F handler) {
  onRegexAsync(method, portPrefix(port) + pattern, std::move(handler));
}

// 安装端口前缀改写（必须在 run() 之前调用一次）。
void installPortRouting();

// 统一 404/405 等框架级错误页：JSON 错误体，与业务响应同构。
void installErrorPages();

void listen(int port);

void setWorkerThreads(std::size_t count);

// 框架日志级别（WARN：避免把整条 URL 的凭据打进日志）。
void setQuietLogging();

} // namespace http_server

#endif // HTTP_SERVER_H