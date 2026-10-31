#ifndef MEDIA_LIBRARY_MEDIA_RUNTIME_H
#define MEDIA_LIBRARY_MEDIA_RUNTIME_H

#include "media_library/artwork.h"
#include "media_library/entity_link.h"
#include "media_library/library.h"
#include "media_library/media_router.h"
#include "media_library/playback.h"
#include "media_library/registry.h"
#include "media_library/snapshot.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace media_library {

// 组合根：5.2 注册表 / 5.3 映射与优先级 / 5.3 路由 / 5.4 门面只在这里组装一次。
// 其它模块一律通过下面四个访问器取用，不自己 new 一套。
struct MediaRuntimeOptions {
  std::string library_id;
  std::string display_name;
  PlatformKind platform = PlatformKind::Local;
  int priority = 100;
  bool enabled = true;
  LibraryLimits limits;
  // 注册时的初始健康状态（未登录的网易云直接标 Unauthenticated）。
  HealthState initial_health = HealthState::Ok;
  // 初始健康的原因（如未登录 = AuthRequired）。
  std::optional<ErrorCode> initial_health_reason;
};

// 一个待注册的库：顺序即注册顺序（AspectPriority 的内建顺序仍由 platform/priority 决定）。
struct LibrarySetup {
  std::shared_ptr<Library> library;
  MediaRuntimeOptions options;
};

// 多库：一次性组装注册表、映射、路由、播放与封面服务。
// 任一库注册失败即抛异常（调用方应事先跳过构造失败的库）。
// 重复调用会先丢弃旧运行期（不关闭传入对象）。
void initialize_media_runtime(std::vector<LibrarySetup> libraries);

// 兼容旧调用（等价于只注册一个库）。
void initialize_media_runtime(std::shared_ptr<Library> library,
                              const MediaRuntimeOptions &options);

bool media_runtime_initialized() noexcept;

// 返回空表示运行期尚未初始化。HTTP 层用这个判断，避免异常穿过请求处理。
std::shared_ptr<MediaRouter> media_router_ptr() noexcept;
std::shared_ptr<LibraryRegistry> library_registry_ptr() noexcept;
std::shared_ptr<EntityLinkResolver> entity_links_ptr() noexcept;
std::shared_ptr<PlaybackService> playback_service_ptr() noexcept;
std::shared_ptr<ArtworkService> artwork_service_ptr() noexcept;

// 未初始化属于程序错误（与 library_runtime::library() 一致）。
MediaRouter &media_router();
LibraryRegistry &library_registry();
EntityLinkResolver &entity_links();
PlaybackService &playback_service();
ArtworkService &artwork_service();

// —— 过渡期兼容访问器（TODO(N6): 跨库聚合完成后删除）
//
// Jellyfin 门面仍在用「库内部 int id」拼兼容 ID，也仍在读本地库快照做列表。
// N1 的目标是让门面不再直接持有 LocalLibrary 对象，所以把这些读操作收敛到
// 组合根：内部 dynamic_cast 到本地库实现，找不到就返回失败/空。
std::optional<SourceRef> compat_ref_for(EntityType type, int internal_id);
std::optional<int> compat_internal_id_of(const SourceRef &ref);
std::optional<TrackTransport> compat_track_transport(int internal_id);
bool compat_snapshot(CatalogSnapshot &out);

// 只释放本运行期持有的引用；Library 对象本身的关闭由调用方负责。
void shutdown_media_runtime() noexcept;

} // namespace media_library

#endif // MEDIA_LIBRARY_MEDIA_RUNTIME_H
