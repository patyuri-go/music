#include "http_stream_buffer.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace http_client {
namespace {

// 即使调用方要求更小的水位，也至少留一块的空间，否则 0 水位会永远等不到。
constexpr std::size_t kMinHighWater = 64 * 1024;

} // namespace

StreamBuffer::StreamBuffer(std::size_t high_water_bytes,
                           std::size_t max_chunk_bytes)
    : high_water_(std::max(high_water_bytes, kMinHighWater)),
      max_chunk_bytes_(max_chunk_bytes) {}

bool StreamBuffer::push(const char *data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return true;
  }
  std::size_t consumed = 0;
  while (consumed < size) {
    const std::size_t piece =
        max_chunk_bytes_ == 0
            ? size - consumed
            : std::min(max_chunk_bytes_, size - consumed);
    {
      std::unique_lock<std::mutex> lock(mutex_);
      // 水位是软上限：等「当前占用低于水位」再入队，避免一次性写满。
      space_cv_.wait(lock, [this] {
        return cancelled_ || buffered_ < high_water_;
      });
      if (cancelled_) {
        return false;
      }
      chunks_.emplace_back(data + consumed, piece);
      buffered_ += piece;
    }
    consumed += piece;
    data_cv_.notify_one();
  }
  return true;
}

void StreamBuffer::finish(bool failed) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    finished_ = true;
    failed_ = failed || failed_;
  }
  data_cv_.notify_all();
  space_cv_.notify_all();
}

void StreamBuffer::cancel() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
  }
  // 两端都要醒：消费者在等数据，生产者在等空间。
  data_cv_.notify_all();
  space_cv_.notify_all();
}

bool StreamBuffer::cancelled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cancelled_;
}

long long StreamBuffer::pop(char *out, std::size_t max_bytes) {
  if (out == nullptr || max_bytes == 0) {
    return 0;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  data_cv_.wait(lock, [this] {
    return !chunks_.empty() || finished_ || cancelled_;
  });
  if (cancelled_) {
    return -1;
  }
  if (chunks_.empty()) {
    // finished_ 且队列空：正常结束；上游传输出错则如实上报。
    return failed_ ? -1 : 0;
  }
  std::string chunk = std::move(chunks_.front());
  chunks_.pop_front();
  const std::size_t take = std::min<std::size_t>(max_bytes, chunk.size());
  std::memcpy(out, chunk.data(), take);
  buffered_ -= take;
  if (take < chunk.size()) {
    // 没吃完的塞回队头；它仍占着字节数。
    chunks_.emplace_front(chunk.substr(take));
  }
  space_cv_.notify_one();
  return static_cast<long long>(take);
}

std::size_t StreamBuffer::buffered() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return buffered_;
}

} // namespace http_client
