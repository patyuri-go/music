// http_client::StreamBuffer 的单元测试。
//
// 只压「有界缓冲」的语义：水位阻塞、取消唤醒、EOF 与传输错误的区分。
// 不碰网络、不起 curl。

#include "http_stream_buffer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using http_client::StreamBuffer;

using namespace std::chrono_literals;

// 等一个条件成立，最多等 timeout；避免用固定 sleep 造成 flaky。
template <typename Predicate>
bool wait_until(Predicate pred, std::chrono::milliseconds timeout = 2000ms) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return pred();
}

std::vector<char> drain(StreamBuffer &buffer, std::size_t max_bytes,
                        std::size_t expected) {
  std::vector<char> out(expected);
  std::size_t got = 0;
  while (got < expected) {
    const long long n =
        buffer.pop(out.data() + got, std::min(max_bytes, expected - got));
    if (n <= 0) {
      break;
    }
    got += static_cast<std::size_t>(n);
  }
  out.resize(got);
  return out;
}

// --------------------------------------------------------------------------

TEST(HttpStreamBufferTest, PopReturnsPushedBytesInOrder) {
  StreamBuffer buffer(64 * 1024);
  const std::string payload = "0123456789abcdef";
  EXPECT_TRUE(buffer.push(payload.data(), payload.size()));
  EXPECT_EQ(buffer.buffered(), payload.size());

  char out[16] = {};
  const long long n = buffer.pop(out, sizeof(out));
  ASSERT_EQ(n, static_cast<long long>(payload.size()));
  EXPECT_EQ(std::string(out, static_cast<std::size_t>(n)), payload);
  EXPECT_EQ(buffer.buffered(), 0u);
}

TEST(HttpStreamBufferTest, PopBlocksUntilDataArrives) {
  StreamBuffer buffer(64 * 1024);
  std::atomic<bool> popped{false};
  std::string got;

  std::thread reader([&] {
    char out[64] = {};
    const long long n = buffer.pop(out, sizeof(out));
    if (n > 0) {
      got.assign(out, static_cast<std::size_t>(n));
      popped = true;
    }
  });

  // 消费者先进入等待，再喂数据；断言它能被唤醒而不是空转返回。
  std::this_thread::sleep_for(20ms);
  EXPECT_FALSE(popped.load());
  ASSERT_TRUE(buffer.push("hello", 5));
  EXPECT_TRUE(wait_until([&] { return popped.load(); }));
  reader.join();
  EXPECT_EQ(got, "hello");
}

// 关键语义：写满水位后生产者必须阻塞，而不是继续往队列里灌。
TEST(HttpStreamBufferTest, WriterBlocksAtHighWaterMark) {
  constexpr std::size_t kWater = 64 * 1024; // StreamBuffer 的下限
  StreamBuffer buffer(kWater, 4096);

  // 写满水位（+1 块会阻塞，所以只写到刚好到水位）。
  const std::string block(kWater, 'x');
  ASSERT_TRUE(buffer.push(block.data(), block.size()));
  EXPECT_EQ(buffer.buffered(), kWater);

  std::atomic<bool> pushed{false};
  std::thread writer([&] {
    // 这一块会让 buffered_ 超过水位，push 内部应先等在 space_cv_ 上。
    if (buffer.push("extra", 5)) {
      pushed = true;
    }
  });

  // 给足时间确认它确实被挡住了。
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(pushed.load()) << "push 超过水位后不应立即返回";

  // 消费一部分，腾出空间，生产者应被唤醒。
  std::vector<char> sink(1024);
  EXPECT_GT(buffer.pop(sink.data(), sink.size()), 0);
  EXPECT_TRUE(wait_until([&] { return pushed.load(); }));
  writer.join();
}

TEST(HttpStreamBufferTest, CancelReleasesBlockedWriter) {
  constexpr std::size_t kWater = 64 * 1024;
  StreamBuffer buffer(kWater, 4096);
  const std::string block(kWater, 'x');
  ASSERT_TRUE(buffer.push(block.data(), block.size()));

  std::atomic<bool> push_returned{false};
  std::atomic<bool> push_result{true};
  std::thread writer([&] {
    push_result = buffer.push("extra", 5);
    push_returned = true;
  });

  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(push_returned.load());

  buffer.cancel();
  EXPECT_TRUE(wait_until([&] { return push_returned.load(); }));
  writer.join();
  // 取消后 push 必须如实返回 false，让 curl 写回调中止上游。
  EXPECT_FALSE(push_result.load());
}

TEST(HttpStreamBufferTest, CancelUnblocksPendingRead) {
  StreamBuffer buffer(64 * 1024);
  std::atomic<long long> result{1};

  std::thread reader([&] {
    char out[64] = {};
    result = buffer.pop(out, sizeof(out));
  });

  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(result.load(), 1) << "pop 应仍在等待";
  buffer.cancel();
  EXPECT_TRUE(wait_until([&] { return result.load() != 1; }));
  reader.join();
  EXPECT_EQ(result.load(), -1);
}

TEST(HttpStreamBufferTest, FinishUnblocksReaderWithEof) {
  StreamBuffer buffer(64 * 1024);
  ASSERT_TRUE(buffer.push("abc", 3));
  buffer.finish(/*failed=*/false);

  EXPECT_EQ(drain(buffer, 16, 3).size(), 3u);
  char out[8] = {};
  EXPECT_EQ(buffer.pop(out, sizeof(out)), 0) << "正常结束后应返回 0";
}

// 传输错误不能被当成正常 EOF：调用方要能据此 cancel + 断连，避免把
// 「截断的音频」当完整文件发给客户端。
TEST(HttpStreamBufferTest, TransportErrorIsNotEof) {
  StreamBuffer buffer(64 * 1024);
  ASSERT_TRUE(buffer.push("abc", 3));
  buffer.finish(/*failed=*/true);

  char out[8] = {};
  ASSERT_EQ(buffer.pop(out, sizeof(out)), 3);
  EXPECT_EQ(buffer.pop(out, sizeof(out)), -1)
      << "上游传输出错后 pop 应返回 -1 而不是 0";
}

TEST(HttpStreamBufferTest, CancelIsIdempotent) {
  StreamBuffer buffer(64 * 1024);
  buffer.cancel();
  buffer.cancel();
  buffer.finish(false);
  EXPECT_TRUE(buffer.cancelled());
  char out[4] = {};
  EXPECT_EQ(buffer.pop(out, sizeof(out)), -1);
}

// 单块超过水位时不应该把整块硬塞进去（否则水位形同虚设）。
TEST(HttpStreamBufferTest, LargeChunkIsSplitAtChunkLimit) {
  StreamBuffer buffer(64 * 1024, 1024);
  const std::string big(4096, 'y');
  ASSERT_TRUE(buffer.push(big.data(), big.size()));
  // 拆成 4 块，总量仍然正确。
  EXPECT_EQ(buffer.buffered(), big.size());
  EXPECT_EQ(drain(buffer, 4096, big.size()), std::vector<char>(big.begin(), big.end()));
}

} // namespace
