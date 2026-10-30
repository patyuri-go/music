#ifndef MEDIA_LIBRARY_MEDIA_RUNTIME_H
#define MEDIA_LIBRARY_MEDIA_RUNTIME_H

#include "media_library/entity_link.h"
#include "media_library/library.h"
#include "media_library/media_router.h"
#include "media_library/playback.h"
#include "media_library/registry.h"

#include <memory>
#include <string>

namespace media_library {

// 组合根：5.2 注册表 / 5.3 映射与优先级 / 5.3 路由 / 5.4 门面只在这里组装一次。
// 其它模块一律通过下面四个访问器取用，不自己 new 一套。
struct MediaRuntimeOptions {
  std::string library_id;
  std::string display_name;
  PlatformKind platform = PlatformKind::Local;
  int priority = 100;
  bool enabled = true;
};

// 把一个已经 initialize() 过的 Library 注册进新的运行期。
// 重复调用会先丢弃旧运行期（不关闭传入对象）。
void initialize_media_runtime(std::shared_ptr<Library> library,
                              const MediaRuntimeOptions &options);

bool media_runtime_initialized() noexcept;

// 返回空表示运行期尚未初始化。HTTP 层用这个判断，避免异常穿过请求处理。
std::shared_ptr<MediaRouter> media_router_ptr() noexcept;
std::shared_ptr<LibraryRegistry> library_registry_ptr() noexcept;
std::shared_ptr<EntityLinkResolver> entity_links_ptr() noexcept;
std::shared_ptr<PlaybackService> playback_service_ptr() noexcept;

// 未初始化属于程序错误（与 library_runtime::library() 一致）。
MediaRouter &media_router();
LibraryRegistry &library_registry();
EntityLinkResolver &entity_links();
PlaybackService &playback_service();

// 只释放本运行期持有的引用；Library 对象本身的关闭由调用方负责。
void shutdown_media_runtime() noexcept;

} // namespace media_library

#endif // MEDIA_LIBRARY_MEDIA_RUNTIME_H
