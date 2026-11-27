#ifndef HTTP_STREAM_BUFFER_H
#define HTTP_STREAM_BUFFER_H

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

namespace http_client {

// 生产者（libcurl 写回调）与消费者（ByteStream::read）之间的有界字节缓冲。
//
// 这是让「流式传输不缓存整段」真正成立的关键：没有水位时，消费者慢一点，
// 生产者就会把整个远端文件灌进内存。有了水位，生产者写满就阻塞，libcurl
// 随之停止从 socket 读，TCP 窗口把背压一路传回上游。
//
// 水位是「软上限」：判定发生在入队之前，所以瞬时占用最多是
// high_water_bytes + 一个 chunk，不会无界增长。
class StreamBuffer {
public:
  explicit StreamBuffer(std::size_t high_water_bytes,
                        std::size_t max_chunk_bytes = 0);
  StreamBuffer(const StreamBuffer &) = delete;
  StreamBuffer &operator=(const StreamBuffer &) = delete;

  // 生产者入队。返回 false 表示已被取消，调用方应中止上游传输
  // （libcurl 写回调返回 0 即可）。
  bool push(const char *data, std::size_t size);

  // 生产者收尾。`failed` 表示上游传输出错（而不是正常读完）。
  void finish(bool failed);

  // 取消：唤醒两端。幂等，可从任意线程调用。
  void cancel();
  bool cancelled() const;

  // 消费者取一块。返回 -1 取消，0 正常结束，>0 读到的字节数。
  // 阻塞直到有数据、流结束或被取消。
  long long pop(char *out, std::size_t max_bytes);

  // 已在队列里、尚未被消费的字节数。
  std::size_t buffered() const;
  std::size_t high_water() const { return high_water_; }

private:
  mutable std::mutex mutex_;
  std::condition_variable data_cv_;  // 消费者等数据
  std::condition_variable space_cv_; // 生产者等空间
  std::deque<std::string> chunks_;
  std::size_t buffered_ = 0;
  std::size_t high_water_;
  std::size_t max_chunk_bytes_;
  bool finished_ = false;
  bool cancelled_ = false;
  bool failed_ = false;
};

} // namespace http_client

#endif // HTTP_STREAM_BUFFER_H
