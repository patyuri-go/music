// stream_http::plan_range 的单元测试。
//
// 这套规则是唯一对外契约（HEAD/GET × Range × 长度已知/未知 × 可 Range），
// 所以用表驱动把组合钉死。不碰 Drogon、不碰网络。

#include "stream_plan.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using stream_http::plan_range;
using stream_http::RangePlan;

constexpr std::int64_t kTotal = 1000;

RangePlan whole(std::int64_t total, bool head = false) {
  return plan_range("", total, true, head);
}

// --------------------------------------------------------------------------
// 无 Range

TEST(StreamPlanTest, NoRangeIsWhole200WithLength) {
  const RangePlan plan = whole(kTotal);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Whole);
  EXPECT_EQ(plan.start, 0);
  EXPECT_EQ(plan.end, 999);
  EXPECT_EQ(plan.length, 1000);
  EXPECT_FALSE(plan.send_content_range);
}

TEST(StreamPlanTest, NoRangeUnknownLengthCannotPromiseLength) {
  const RangePlan plan = whole(-1);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Whole);
  EXPECT_EQ(plan.length, -1);
  EXPECT_EQ(plan.total, -1);
}

TEST(StreamPlanTest, NoRangeOnEmptyFileIsWholeZero) {
  const RangePlan plan = whole(0);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Whole);
  EXPECT_EQ(plan.length, 0);
}

// --------------------------------------------------------------------------
// HEAD

TEST(StreamPlanTest, HeadIsAlwaysWholeEvenWithRange) {
  const RangePlan plan = plan_range("bytes=10-20", kTotal, true, true);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Whole) << "HEAD 不适用 206/416";
  EXPECT_EQ(plan.start, 0);
  EXPECT_EQ(plan.length, kTotal);
  EXPECT_TRUE(plan.head_only);
}

TEST(StreamPlanTest, HeadUnknownLengthKeepsLengthUnknown) {
  const RangePlan plan = plan_range("bytes=0-", -1, true, true);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Whole);
  EXPECT_EQ(plan.length, -1);
}

// --------------------------------------------------------------------------
// 正常 Range

TEST(StreamPlanTest, ClosedRange) {
  const RangePlan plan = plan_range("bytes=10-19", kTotal, true, false);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Partial);
  EXPECT_EQ(plan.start, 10);
  EXPECT_EQ(plan.end, 19);
  EXPECT_EQ(plan.length, 10);
  EXPECT_TRUE(plan.send_content_range);
}

TEST(StreamPlanTest, OpenEndedRangeRunsToEnd) {
  const RangePlan plan = plan_range("bytes=900-", kTotal, true, false);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Partial);
  EXPECT_EQ(plan.start, 900);
  EXPECT_EQ(plan.end, 999);
  EXPECT_EQ(plan.length, 100);
}

TEST(StreamPlanTest, SuffixRangeTakesLastBytes) {
  const RangePlan plan = plan_range("bytes=-50", kTotal, true, false);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Partial);
  EXPECT_EQ(plan.start, 950);
  EXPECT_EQ(plan.end, 999);
  EXPECT_EQ(plan.length, 50);
}

TEST(StreamPlanTest, SuffixLargerThanFileClampsToWhole) {
  const RangePlan plan = plan_range("bytes=-5000", kTotal, true, false);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Partial);
  EXPECT_EQ(plan.start, 0);
  EXPECT_EQ(plan.end, 999);
  EXPECT_EQ(plan.length, 1000);
}

TEST(StreamPlanTest, EndBeyondFileClampsToLastByte) {
  const RangePlan plan = plan_range("bytes=900-99999", kTotal, true, false);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Partial);
  EXPECT_EQ(plan.start, 900);
  EXPECT_EQ(plan.end, 999);
  EXPECT_EQ(plan.length, 100);
}

TEST(StreamPlanTest, StartBeyondFileIsUnsatisfiable) {
  const RangePlan plan = plan_range("bytes=1000-", kTotal, true, false);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Unsatisfiable);
  EXPECT_EQ(plan.total, kTotal);
  EXPECT_TRUE(plan.send_content_range);
}

TEST(StreamPlanTest, RangeCoveringWholeFileIsStill206) {
  // 请求了 Range 就是 206，哪怕区间正好是整个文件。
  const RangePlan plan = plan_range("bytes=0-999", kTotal, true, false);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Partial);
  EXPECT_EQ(plan.length, 1000);
}

// --------------------------------------------------------------------------
// 多段只取第一段

TEST(StreamPlanTest, MultipleRangesUsesFirstSegmentOnly) {
  const RangePlan plan = plan_range("bytes=0-9,20-29", kTotal, true, false);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Partial);
  EXPECT_EQ(plan.start, 0);
  EXPECT_EQ(plan.end, 9);
  EXPECT_EQ(plan.length, 10);
}

// --------------------------------------------------------------------------
// 畸形输入：必须是 416，而不是抛异常

TEST(StreamPlanTest, MalformedRangeIsUnsatisfiableNotThrow) {
  for (const char *bad :
       {"bytes=abc-def", "bytes=", "bytes=", "bytes=--5", "items=0-5",
        "bytes=1", "bytes=  ", "bytes=+1-2"}) {
    EXPECT_NO_THROW({
      const RangePlan plan = plan_range(bad, kTotal, true, false);
      EXPECT_EQ(plan.mode, RangePlan::Mode::Unsatisfiable) << bad;
    }) << bad;
  }
}

TEST(StreamPlanTest, WhitespaceAroundSpecIsTolerated) {
  const RangePlan plan = plan_range(" bytes = 10 - 19 ", kTotal, true, false);
  // HTTP 头可能带空白；按 RFC 允许的 OWS 处理。
  EXPECT_EQ(plan.mode, RangePlan::Mode::Partial);
  EXPECT_EQ(plan.start, 10);
  EXPECT_EQ(plan.end, 19);
}

TEST(StreamPlanTest, ZeroSuffixIsUnsatisfiable) {
  const RangePlan plan = plan_range("bytes=-0", kTotal, true, false);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Unsatisfiable);
}

TEST(StreamPlanTest, RangeOnEmptyFileIsUnsatisfiable) {
  const RangePlan plan = plan_range("bytes=0-0", 0, true, false);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Unsatisfiable);
  EXPECT_EQ(plan.total, 0);
}

// --------------------------------------------------------------------------
// 总长未知

TEST(StreamPlanTest, UnknownTotalRangeIs206WithoutContentRange) {
  const RangePlan plan = plan_range("bytes=100-", -1, true, false);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Partial);
  EXPECT_EQ(plan.start, 100);
  EXPECT_EQ(plan.end, -1);
  EXPECT_EQ(plan.length, -1);
  EXPECT_FALSE(plan.send_content_range) << "总长未知时不能发 Content-Range";
}

TEST(StreamPlanTest, UnknownTotalSuffixRangeFallsBackToWhole) {
  // 无从定位「最后 N 字节」，整段输出比猜一个区间更诚实。
  const RangePlan plan = plan_range("bytes=-50", -1, true, false);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Whole);
}

TEST(StreamPlanTest, UnknownTotalInvertedRangeIsUnsatisfiable) {
  const RangePlan plan = plan_range("bytes=100-50", -1, true, false);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Unsatisfiable);
  EXPECT_FALSE(plan.send_content_range);
}

// --------------------------------------------------------------------------
// 源不支持 Range

TEST(StreamPlanTest, UnseekableSourceIgnoresRange) {
  const RangePlan plan = plan_range("bytes=10-19", kTotal, false, false);
  EXPECT_EQ(plan.mode, RangePlan::Mode::Whole) << "range_supported=false 时忽略 Range";
  EXPECT_EQ(plan.start, 0);
  EXPECT_EQ(plan.length, kTotal);
}

} // namespace
