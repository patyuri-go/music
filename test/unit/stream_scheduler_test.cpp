// stream_http::Scheduler 的单元测试。
//
// 压的是「池的语义」：两池互不阻塞、队列满返回 false、关停丢弃排队任务、
// 关停等已开始的任务收尾。不碰网络、不碰 Drogon。

#include "stream_scheduler.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using stream_http::Scheduler;
using stream_http::SchedulerOptions;

using namespace std::chrono_literals;

// Scheduler 是进程内单例，测试必须串行使用：每个用例自己 start/stop。
class SchedulerScope {
public:
  explicit SchedulerScope(const SchedulerOptions &options) {
    Scheduler::instance().start(options);
  }
  ~SchedulerScope() { Scheduler::instance().stop(); }
  SchedulerScope(const SchedulerScope &) = delete;
  SchedulerScope &operator=(const SchedulerScope &) = delete;
};

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

SchedulerOptions options_with(int audio_workers, int audio_queue,
                              int image_workers, int image_queue) {
  SchedulerOptions options;
  options.audio_workers = audio_workers;
  options.audio_queue = audio_queue;
  options.image_workers = image_workers;
  options.image_queue = image_queue;
  return options;
}

TEST(StreamSchedulerTest, RunsSubmittedTasks) {
  SchedulerScope scope(options_with(2, 8, 1, 8));
  std::atomic<int> audio{0};
  std::atomic<int> image{0};

  ASSERT_TRUE(Scheduler::instance().submit_audio([&] { ++audio; }));
  ASSERT_TRUE(Scheduler::instance().submit_image([&] { ++image; }));

  EXPECT_TRUE(wait_until([&] { return audio.load() == 1 && image.load() == 1; }));
}

// 两池独立：音频池被占满时，图片任务仍能立刻跑。这是 Q5/Q14 的核心。
TEST(StreamSchedulerTest, AudioAndImagePoolsAreIndependent) {
  SchedulerScope scope(options_with(1, 4, 1, 4));

  std::mutex mutex;
  std::condition_variable cv;
  bool release = false;
  std::atomic<bool> audio_started{false};

  // 占住唯一的音频 worker。
  ASSERT_TRUE(Scheduler::instance().submit_audio([&] {
    audio_started = true;
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return release; });
  }));
  ASSERT_TRUE(wait_until([&] { return audio_started.load(); }));

  // 音频池此时全忙，但图片池必须照常工作。
  std::atomic<bool> image_ran{false};
  ASSERT_TRUE(Scheduler::instance().submit_image([&] { image_ran = true; }));
  EXPECT_TRUE(wait_until([&] { return image_ran.load(); }))
      << "图片池不应被音频池的忙状态阻塞";

  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  cv.notify_all();
}

TEST(StreamSchedulerTest, QueueFullRejectsInsteadOfBlocking) {
  // 1 个 worker、1 个队列位：第 1 个在跑，第 2 个入队，第 3 个必须被拒。
  SchedulerScope scope(options_with(1, 1, 1, 1));

  std::mutex mutex;
  std::condition_variable cv;
  bool release = false;
  std::atomic<bool> started{false};

  ASSERT_TRUE(Scheduler::instance().submit_audio([&] {
    started = true;
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return release; });
  }));
  ASSERT_TRUE(wait_until([&] { return started.load(); }));

  ASSERT_TRUE(Scheduler::instance().submit_audio([] {})) << "队列还有 1 个空位";
  EXPECT_FALSE(Scheduler::instance().submit_audio([] {}))
      << "队列满时必须返回 false，由调用方回 503";

  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  cv.notify_all();
}

TEST(StreamSchedulerTest, RejectsWhenNotStarted) {
  Scheduler::instance().stop();
  EXPECT_FALSE(Scheduler::instance().submit_audio([] {}));
  EXPECT_FALSE(Scheduler::instance().submit_image([] {}));
}

// 关停时已排队但未开始的任务按设计丢弃（Q27），不能偷偷执行。
TEST(StreamSchedulerTest, ShutdownDropsQueuedTasks) {
  Scheduler::instance().start(options_with(1, 4, 1, 4));

  std::atomic<bool> started{false};
  std::atomic<int> queued_ran{0};

  // 唯一的 worker 先忙 150ms，后面两个任务只能排在队列里。
  ASSERT_TRUE(Scheduler::instance().submit_audio([&] {
    started = true;
    std::this_thread::sleep_for(150ms);
  }));
  ASSERT_TRUE(wait_until([&] { return started.load(); }));

  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(Scheduler::instance().submit_audio([&] { ++queued_ran; }));
  }

  // stop() 先清空队列，再 join 正在睡的那个 worker；因此返回时排队任务
  // 已经被丢弃，不可能再被执行。无需额外的时序让步。
  Scheduler::instance().stop();

  EXPECT_EQ(queued_ran.load(), 0) << "关停时未开始的任务应被丢弃";
}

// 关停必须等已开始的任务收尾，不能直接丢掉正在写响应的 worker。
TEST(StreamSchedulerTest, ShutdownWaitsForInFlight) {
  Scheduler::instance().start(options_with(1, 4, 1, 4));
  std::atomic<bool> started{false};
  std::atomic<bool> finished{false};

  ASSERT_TRUE(Scheduler::instance().submit_audio([&] {
    started = true;
    std::this_thread::sleep_for(80ms);
    finished = true;
  }));
  // 等它真正开始执行；否则 stop() 会把它当「排队中」直接丢弃。
  ASSERT_TRUE(wait_until([&] { return started.load(); }));

  Scheduler::instance().stop();
  EXPECT_TRUE(finished.load()) << "stop() 返回时 in-flight 任务应已完成";
}

TEST(StreamSchedulerTest, StartIsIdempotent) {
  SchedulerScope scope(options_with(1, 1, 1, 1));
  // 二次 start 不应换池、不应丢任务。
  Scheduler::instance().start(options_with(4, 100, 4, 100));
  std::atomic<bool> ran{false};
  ASSERT_TRUE(Scheduler::instance().submit_audio([&] { ran = true; }));
  EXPECT_TRUE(wait_until([&] { return ran.load(); }));
}

} // namespace
