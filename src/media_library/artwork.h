#ifndef MEDIA_LIBRARY_ARTWORK_H
#define MEDIA_LIBRARY_ARTWORK_H

#include "media_library/byte_stream.h"
#include "media_library/domain.h"
#include "media_library/error.h"
#include "media_library/library.h"
#include "media_library/media_router.h"
#include "media_library/registry.h"

#include <cstdint>
#include <memory>
#include <string>

namespace media_library {

// 封面元信息。只描述，不含字节。
struct ArtworkInfo {
  std::string mime_type;          // image/jpeg | image/png | ...；未知时为空
  std::int64_t size_bytes = -1;   // -1 = 未知（不承诺 Range）
  std::string etag;               // 来自 ArtworkRef::cache_tag
  std::string library_id;         // 仅用于 X-Musicd-Origin 归因，不属于领域模型
};

struct ArtworkServiceOptions {
  // 目前没有可调项；保留结构以便后续加超时/并发上限。
};

// 应用层出图服务，与 PlaybackService 对称。
//
// 它不持有字节：`open` 返回一个 ByteStream，由 stream_http 按 Range 语义
// 边读边写。封面可能是数 MB 的原图（本地库）或未知大小的远端图，整块
// 读进内存既慢又占 RSS。
//
// `ArtworkLocation::locator` 只在本服务内部使用，绝不出进程。
class ArtworkService {
public:
  virtual ~ArtworkService() = default;

  virtual Result<ArtworkInfo> info(const ArtworkRef &ref,
                                   const CallContext &ctx) = 0;

  // 所有权交给调用方。不做进程内字节缓存。
  virtual Result<std::unique_ptr<ByteStream>>
  open(const ArtworkRef &ref, const CallContext &ctx) = 0;
};

std::shared_ptr<ArtworkService>
make_artwork_service(std::shared_ptr<MediaRouter> router,
                     std::shared_ptr<LibraryRegistry> registry,
                     const ArtworkServiceOptions &options = {});

} // namespace media_library

#endif // MEDIA_LIBRARY_ARTWORK_H
