#ifndef STREAM_SCHEDULER_H
#define STREAM_SCHEDULER_H

#include <cstddef>
#include <cstdint>
#include <functional>

// 音频与封面各自的阻塞任务线程池。
//
// 为什么需要它：把 locator 变成字节要先连上游、探总长、再逐块读。这些都会
// 阻塞。以前每个请求 detach 一个线程（无池、无界），慢请求一多就把线程数和
// 内存都撑爆。现在按用途分成两个池，队列有界，满了直接回 503 让客户端重试。
namespace stream_http {

struct SchedulerOptions {
  int audio_workers = 4;
  int audio_queue = 100;
  int image_workers = 2;
  int image_queue = 100;
};

class Scheduler {
public:
  // 进程内唯一实例；必须在 drogon::app().run() 之前 start()。
  static Scheduler &instance();

  void start(const SchedulerOptions &options);
  // 拒绝新任务，等已开始的收尾；已入队但未开始的任务直接丢弃。
  void stop();

  // 提交一个阻塞任务。返回 false 表示队列已满（调用方应回 503 +
  // Retry-After），此时 `task` 不会被调用。
  bool submit_audio(std::function<void()> task);
  bool submit_image(std::function<void()> task);

  bool running() const;

private:
  Scheduler();
  ~Scheduler();
  Scheduler(const Scheduler &) = delete;
  Scheduler &operator=(const Scheduler &) = delete;

  struct Impl;
  Impl *impl_;
};

} // namespace stream_http

#endif // STREAM_SCHEDULER_H
