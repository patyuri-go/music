#include "stream_plan.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <string_view>

namespace stream_http {
namespace {

// 从 `view` 里解析一个非负十进制整数。失败（空、非数字、溢出、带多余字符）
// 返回 nullopt。不用 std::stoll：它对畸形输入抛异常，而 HTTP 头是外部输入，
// 抛异常会把一次坏请求变成 500。
std::optional<std::int64_t> parse_uint(std::string_view view) {
  if (view.empty()) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  const auto *begin = view.data();
  const auto *end = begin + view.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end || value < 0) {
    return std::nullopt;
  }
  return value;
}

std::string_view trim(std::string_view view) {
  while (!view.empty() &&
         (view.front() == ' ' || view.front() == '\t')) {
    view.remove_prefix(1);
  }
  while (!view.empty() &&
         (view.back() == ' ' || view.back() == '\t' || view.back() == '\r')) {
    view.remove_suffix(1);
  }
  return view;
}

RangePlan whole_plan(std::int64_t total, bool head_only) {
  RangePlan plan;
  plan.mode = RangePlan::Mode::Whole;
  plan.start = 0;
  plan.end = total >= 0 ? total - 1 : -1;
  plan.length = total;
  plan.total = total;
  plan.send_content_range = false;
  plan.head_only = head_only;
  return plan;
}

} // namespace

RangePlan plan_range(const std::string &range_header, std::int64_t total,
                     bool seekable, bool head_only) {
  // HEAD 永远 200 + 完整长度：不适用 416，也不做 Range 解析。
  if (head_only) {
    return whole_plan(total, true);
  }
  // 源不支持 Range：忽略 Range 头，整段输出（Q24 兜底）。
  if (!seekable) {
    return whole_plan(total, false);
  }
  const std::string_view raw = trim(range_header);
  if (raw.empty()) {
    return whole_plan(total, false);
  }

  // 空文件上的任何 Range 都无法满足。
  if (total == 0) {
    RangePlan plan;
    plan.mode = RangePlan::Mode::Unsatisfiable;
    plan.total = 0;
    plan.send_content_range = true;
    return plan;
  }

  constexpr std::string_view kUnit = "bytes";
  if (raw.size() <= kUnit.size() ||
      raw.substr(0, kUnit.size()) != kUnit) {
    RangePlan plan;
    plan.mode = RangePlan::Mode::Unsatisfiable;
    plan.total = total;
    plan.send_content_range = total >= 0;
    return plan;
  }
  std::string_view after_unit = trim(raw.substr(kUnit.size()));
  if (after_unit.empty() || after_unit.front() != '=') {
    // 非 bytes 单位（如 items=）或语法不完整：按「不可解析」处理。
    RangePlan plan;
    plan.mode = RangePlan::Mode::Unsatisfiable;
    plan.total = total;
    plan.send_content_range = total >= 0;
    return plan;
  }

  std::string_view spec = trim(after_unit.substr(1));
  // 多段只取第一段（不做 multipart/byteranges）。
  if (const std::size_t comma = spec.find(',');
      comma != std::string_view::npos) {
    spec = trim(spec.substr(0, comma));
  }
  if (spec.empty()) {
    RangePlan plan;
    plan.mode = RangePlan::Mode::Unsatisfiable;
    plan.total = total;
    plan.send_content_range = total >= 0;
    return plan;
  }

  std::int64_t start = 0;
  std::int64_t end = -1;

  if (spec.front() == '-') {
    // 后缀形式 `-N`：最后 N 字节。N 必须 > 0。
    const auto suffix = parse_uint(spec.substr(1));
    if (!suffix.has_value() || *suffix <= 0) {
      RangePlan plan;
      plan.mode = RangePlan::Mode::Unsatisfiable;
      plan.total = total;
      plan.send_content_range = total >= 0;
      return plan;
    }
    if (total >= 0) {
      const std::int64_t take = std::min(*suffix, total);
      start = total - take;
      end = total - 1;
    } else {
      // 总长未知时无法定位「最后 N 字节」：回整段更诚实。
      return whole_plan(total, false);
    }
  } else {
    const std::size_t dash = spec.find('-');
    if (dash == std::string_view::npos) {
      RangePlan plan;
      plan.mode = RangePlan::Mode::Unsatisfiable;
      plan.total = total;
      plan.send_content_range = total >= 0;
      return plan;
    }
    const auto parsed_start = parse_uint(trim(spec.substr(0, dash)));
    if (!parsed_start.has_value()) {
      RangePlan plan;
      plan.mode = RangePlan::Mode::Unsatisfiable;
      plan.total = total;
      plan.send_content_range = total >= 0;
      return plan;
    }
    start = *parsed_start;
    const std::string_view end_part = trim(spec.substr(dash + 1));
    if (!end_part.empty()) {
      const auto parsed_end = parse_uint(end_part);
      if (!parsed_end.has_value()) {
        RangePlan plan;
        plan.mode = RangePlan::Mode::Unsatisfiable;
        plan.total = total;
        plan.send_content_range = total >= 0;
        return plan;
      }
      end = *parsed_end;
    }

    if (total >= 0) {
      // 起点越界 → 416；末尾越界则夹到 total-1。
      if (start >= total) {
        RangePlan plan;
        plan.mode = RangePlan::Mode::Unsatisfiable;
        plan.total = total;
        plan.send_content_range = true;
        return plan;
      }
      if (end < 0 || end >= total) {
        end = total - 1;
      }
      if (start > end) {
        RangePlan plan;
        plan.mode = RangePlan::Mode::Unsatisfiable;
        plan.total = total;
        plan.send_content_range = true;
        return plan;
      }
    } else {
      // 总长未知：只要 start 可解析就按开放区间处理；end 由流结束决定。
      if (end >= 0 && end < start) {
        RangePlan plan;
        plan.mode = RangePlan::Mode::Unsatisfiable;
        plan.total = -1;
        plan.send_content_range = false;
        return plan;
      }
    }
  }

  RangePlan plan;
  // 只要请求带了可解析的 Range，就是 206（即使区间覆盖整个文件）。
  plan.mode = RangePlan::Mode::Partial;
  plan.start = start;
  plan.end = end;
  plan.total = total;
  // total 已知才敢发 Content-Range；未知时只回 206，不带它。
  plan.send_content_range = total >= 0;
  plan.length = (total >= 0 && end >= 0) ? end - start + 1 : -1;
  return plan;
}

} // namespace stream_http
