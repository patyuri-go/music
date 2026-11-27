#include "stream_scheduler.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace stream_http {
namespace {

// 一个有界任务池。所有状态都在 mutex 后面；worker 只在持锁时碰队列，
// 任务本身在锁外执行（否则一个慢任务会阻塞整个池）。
class Pool {
public:
  Pool(std::string name, int workers, int capacity)
      : name_(std::move(name)), capacity_(static_cast<std::size_t>(capacity)) {
    for (int i = 0; i < workers; ++i) {
      threads_.emplace_back([this] { worker_loop(); });
    }
  }

  ~Pool() { shutdown(); }

  // 返回 false = 队列满。此时 task 未被移动，调用方可安全丢弃。
  bool submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ || queue_.size() >= capacity_) {
        return false;
      }
      queue_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
  }

  // 拒绝新任务；丢弃已排队但未开始的；等已开始的 worker 收尾。
  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        return;
      }
      stopping_ = true;
      queue_.clear(); // 未开始的任务按设计丢弃（Q27）
    }
    cv_.notify_all();
    for (std::thread &thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    threads_.clear();
  }

private:
  void worker_loop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_) {
          return; // 关停：不再取新任务
        }
        task = std::move(queue_.front());
        queue_.pop_front();
        ++in_flight_;
      }
      // 任务在锁外跑：它可能阻塞很久（连上游），不能挡住其他 worker。
      task();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        --in_flight_;
      }
      drained_cv_.notify_all();
    }
  }

  std::string name_;
  std::size_t capacity_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable drained_cv_;
  std::deque<std::function<void()>> queue_;
  std::vector<std::thread> threads_;
  std::size_t in_flight_ = 0;
  bool stopping_ = false;
};

} // namespace

struct Scheduler::Impl {
  std::unique_ptr<Pool> audio;
  std::unique_ptr<Pool> image;
  std::mutex mutex;
  bool running = false;
};

Scheduler &Scheduler::instance() {
  static Scheduler scheduler;
  return scheduler;
}

Scheduler::Scheduler() : impl_(new Impl()) {}

Scheduler::~Scheduler() {
  stop();
  delete impl_;
}

void Scheduler::start(const SchedulerOptions &options) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->running) {
    return;
  }
  impl_->audio = std::make_unique<Pool>(
      "audio", std::max(1, options.audio_workers), std::max(1, options.audio_queue));
  impl_->image = std::make_unique<Pool>(
      "image", std::max(1, options.image_workers), std::max(1, options.image_queue));
  impl_->running = true;
}

void Scheduler::stop() {
  std::unique_ptr<Pool> audio;
  std::unique_ptr<Pool> image;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->running) {
      return;
    }
    impl_->running = false;
    audio = std::move(impl_->audio);
    image = std::move(impl_->image);
  }
  // 在锁外关停：shutdown() 会 join worker，而 worker 可能正在调 stop()。
  if (audio) {
    audio->shutdown();
  }
  if (image) {
    image->shutdown();
  }
}

bool Scheduler::submit_audio(std::function<void()> task) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->running || !impl_->audio) {
    return false;
  }
  return impl_->audio->submit(std::move(task));
}

bool Scheduler::submit_image(std::function<void()> task) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->running || !impl_->image) {
    return false;
  }
  return impl_->image->submit(std::move(task));
}

bool Scheduler::running() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->running;
}

} // namespace stream_http
