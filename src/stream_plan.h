#ifndef STREAM_PLAN_H
#define STREAM_PLAN_H

#include <cstdint>
#include <string>

// Range 语义的纯决策层：只回答「这次请求该回什么状态码、哪一段、
// 带不带 Content-Range / Content-Length」，不碰 Drogon、不碰网络。
//
// 之所以抽出来，是因为这套规则组合很多（HEAD / GET × 有 Range / 无 Range
// × 总长已知 / 未知 × 源可 Range / 不可 Range），散在 serve() 的 if 里
// 既难读也难测。这里是唯一的事实来源，serve() 只做执行。
namespace stream_http {

struct RangePlan {
  enum class Mode {
    Whole,         // 200：整段输出
    Partial,       // 206：输出 [start, end]
    Unsatisfiable, // 416：Range 无法满足或畸形
  };

  Mode mode = Mode::Whole;
  std::int64_t start = 0;
  // 闭区间末尾；-1 表示「直到流结束」（此时长度未知）。
  std::int64_t end = -1;
  // 输出长度；-1 表示未知（不能写 Content-Length，也不能写 Content-Range）。
  std::int64_t length = -1;
  // 源的真实总长；-1 表示未知。
  std::int64_t total = -1;
  // 只在对端能理解「这一段在整体里的位置」时置真。
  bool send_content_range = false;
  // HEAD 请求：只要头，不读字节。此时 mode 恒为 Whole（HEAD 不适用 416）。
  bool head_only = false;
};

// `range_header` 为空表示请求没有 Range。
// `total` < 0 表示总长未知。
// `seekable` 为假表示源明确不支持 Range：忽略 Range，回整段。
RangePlan plan_range(const std::string &range_header, std::int64_t total,
                     bool seekable, bool head_only);

} // namespace stream_http

#endif // STREAM_PLAN_H
