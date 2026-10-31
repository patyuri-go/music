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

  // 返回空数组表示流已结束；返回失败表示读到一半出错。
  virtual Result<std::vector<std::byte>> read(std::size_t max_bytes) = 0;

  virtual Status seek(std::int64_t offset) = 0;
};

// 音频只是「可 seek 的字节流」；保留别名让既有调用点零改动。
using AudioStream = ByteStream;

// 本地文件流。打不开返回 ErrorCode::NotFound。
Result<std::unique_ptr<ByteStream>> make_file_stream(std::string path,
                                                     std::string mime_type);

// 远端（http/https/WebDAV）流。连接惰性建立；seek 靠带 Range 重开连接。
// `size` 未知时传 nullopt 或 -1，运行时从响应头补齐。
// `seekable=false` 表示源明确不支持 Range（此时拒绝非零 seek）。
Result<std::unique_ptr<ByteStream>> make_http_stream(
    std::string locator, std::string mime_type,
    std::optional<std::int64_t> size = std::nullopt, bool seekable = true);

} // namespace media_library

#endif // MEDIA_LIBRARY_BYTE_STREAM_H
