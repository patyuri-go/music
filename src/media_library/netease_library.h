#ifndef MEDIA_LIBRARY_NETEASE_LIBRARY_H
#define MEDIA_LIBRARY_NETEASE_LIBRARY_H

#include "api/api-enhanced/client.h"
#include "api/api-enhanced/transport.h"
#include "media_library/error.h"
#include "media_library/library.h"
#include "media_library/registry.h"

#include <memory>
#include <mutex>
#include <string>

namespace media_library {

// 网易云媒体库对象。
//
// N1 只落地三件事：身份、能力声明、健康探测。曲目/专辑/播放/歌词/搜索
// 在后续阶段逐个打开能力，不做「先声明支持再返回 unsupported」的假承诺。
//
// 与本地库的关键差异：没有 EntityStore。entity_id 就是平台 ID 的十进制
// 文本（见 api/api-enhanced/ids.h），因此 internal_id_of 语义上不成立。
class NeteaseLibrary : public Library,
                       public ArtworkBytes,
                       public std::enable_shared_from_this<NeteaseLibrary> {
public:
  NeteaseLibrary(std::string library_id, std::string display_name,
                 std::shared_ptr<api_enhanced::NeteaseClient> client);

  // 必须在 make_shared 之后调用：内部要 shared_from_this 注册能力。
  void initialize();

  LibraryIdentity identity() const override;
  CapabilitySet capabilities() const override;
  void close() noexcept override;

  // N1 未实现：封面字节出口留到 N4，这里显式 Unsupported 而不是空 locator。
  Result<ArtworkLocation> artwork_location(const ArtworkRef &ref,
                                           const CallContext &ctx) override;

  // 探测一次登录状态并写入内部健康快照，供 media_runtime 作为注册初始健康。
  // 不抛异常：探测失败只是把库标成 Unavailable。
  void probe_health(const CallContext &ctx);

  HealthSnapshot health() const;

private:
  std::string library_id_;
  std::string display_name_;
  std::shared_ptr<api_enhanced::NeteaseClient> client_;

  mutable std::mutex mutex_;
  HealthSnapshot health_;
};

// 构造网易云库：建 transport + client，探测登录状态并落入初始健康。
// 只有 api_base 非法这类「配置错误」才返回 failure；api-enhanced 不可用
// 不是构造失败，而是把库标成 Unavailable 后照常注册。
Result<std::shared_ptr<NeteaseLibrary>>
make_netease_library(const std::string &library_id,
                     const std::string &display_name,
                     const api_enhanced::NeteaseTransportOptions &transport,
                     const api_enhanced::NeteaseClientOptions &client);

} // namespace media_library

#endif // MEDIA_LIBRARY_NETEASE_LIBRARY_H
