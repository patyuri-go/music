#ifndef MEDIA_LIBRARY_BYTE_STREAM_H
#define MEDIA_LIBRARY_BYTE_STREAM_H

#include "media_library/error.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace media_library {

// ---------------------------------------------------------------------------
// 字节流
//
// 只暴露「多大、读一块、跳一处」，不暴露底层是文件、WebDAV 还是远端 URL。
// 音频与图片共用这一个契约：二者的差异只有 mime_type 与 seekable。
// ---------------------------------------------------------------------------

struct StreamInfo {
  std::string mime_type;
  std::int64_t size_bytes = -1; // -1 表示长度未知（不支持 Range）
  bool seekable = true;
};

class ByteStream {
public:
  virtual ~ByteStream() = default;

  virtual StreamInfo info() const = 0;

  // 在真正读字节之前把元信息确定下来。
  //
  // 远端流是惰性连接的：info().size_bytes 在第一次 read 之前可能是 -1。
  // 而 HTTP 层必须先有长度才能给出正确的 Content-Length，否则会按
  // 空 body 补一个 "Content-Length: 0"，客户端因此认为响应体为空。
  // 默认实现什么都不做（本地文件、已知长度的流无需探测）。
  virtual Status prepare() { return Status::success(); }

  // 返回空数组表示流已结束；返回失败表示读到一半出错。
  virtual Result<std::vector<std::byte>> read(std::size_t max_bytes) = 0;

  virtual Status seek(std::int64_t offset) = 0;

  // 中止正在进行的读取，让阻塞中的 read() 尽快返回失败。
  //
  // 用于「客户端断开」或「推送超时」：上游连接不能继续占着。默认空实现
  // （本地文件无需中止）；覆写者必须保证幂等、可从任意线程调用。
  // 显式调用，不要依赖析构 —— 析构时机由响应对象决定，可能太晚。
  virtual void cancel() {}

  // 若这是一个本地文件流，返回它的路径；否则 nullopt。
  //
  // HTTP 层据此走 Drogon 的零拷贝文件响应（Range / 206 / 416 由框架处理），
  // 不必把文件内容读进内存再推送。
  virtual std::optional<std::string> local_path() const {
    return std::nullopt;
  }
};

// 音频只是「可 seek 的字节流」；保留别名让既有调用点零改动。
using AudioStream = ByteStream;

// 本地文件流。打不开返回 ErrorCode::NotFound。
Result<std::unique_ptr<ByteStream>> make_file_stream(std::string path,
                                                     std::string mime_type);

// 远端（http/https/WebDAV）流。连接惰性建立；seek 靠带 Range 重开连接。
// `size` 未知时传 nullopt 或 -1，运行时从响应头补齐。
// `seekable=false` 表示源明确不支持 Range（此时拒绝非零 seek）。
// `open_timeout_ms > 0` 时限制等待响应头的时间；超时按 ErrorCode::Timeout 上报。
// `max_redirects > 0` 时跟随 3xx（只允许 http/https）：存储节点可以把
// 上游直链以 302 交出来，由本进程跟随后再读字节。
// `buffer_bytes` 是生产端（libcurl 写回调）与消费端之间的字节水位，决定
// 「最多能提前下载多少」；0 表示用 defaults 里配置的水位。
Result<std::unique_ptr<ByteStream>> make_http_stream(
    std::string locator, std::string mime_type,
    std::optional<std::int64_t> size = std::nullopt, bool seekable = true,
    std::int64_t open_timeout_ms = 0, int max_redirects = 0,
    std::size_t buffer_bytes = 0);

// 由启动代码注入（来自 config 的 http_stream.upstream_buffer_bytes）。
// 单独放在这里而不是让 media_library 反向依赖 stream_http：字节层只需要
// 一个数字，不需要知道 HTTP 层的存在。未注入时沿用 http_client 的默认值。
void set_upstream_buffer_bytes(std::size_t bytes);

} // namespace media_library

#endif // MEDIA_LIBRARY_BYTE_STREAM_H
