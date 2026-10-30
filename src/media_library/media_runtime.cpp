#include "media_library/media_runtime.h"

#include "media_library/router.h"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace media_library {
namespace {

struct Runtime {
  std::shared_ptr<LibraryRegistry> registry;
  std::shared_ptr<AspectPriority> aspect_priority;
  std::shared_ptr<EntityLinkResolver> links;
  std::shared_ptr<AggregatePriority> aggregate_priority;
  std::shared_ptr<Router> router;
  std::shared_ptr<MediaRouter> media_router;
};

std::mutex &mutex() {
  static std::mutex instance;
  return instance;
}

Runtime &state() {
  static Runtime instance;
  return instance;
}

} // namespace

void initialize_media_runtime(std::shared_ptr<Library> library,
                              const MediaRuntimeOptions &options) {
  if (library == nullptr) {
    throw std::invalid_argument("initialize_media_runtime: library is null");
  }
  if (options.library_id.empty()) {
    throw std::invalid_argument("initialize_media_runtime: library_id is empty");
  }

  Runtime next;
  next.registry = make_library_registry();

  LibraryRegistration registration;
  registration.library_id = options.library_id;
  registration.display_name = options.display_name.empty()
                                  ? options.library_id
                                  : options.display_name;
  registration.platform = options.platform;
  registration.priority = options.priority;
  registration.enabled = options.enabled;

  const Status added = next.registry->add(std::move(library), registration);
  if (!added.ok()) {
    throw std::runtime_error("initialize_media_runtime: " +
                             std::string(error_code_name(added.error().code)));
  }

  // 方面优先级：留空即三层回落的内建顺序（本地优先，其次注册项 priority）。
  next.aspect_priority = make_memory_aspect_priority();
  // 内建兜底顺序从注册表现读，避免两处各存一份库清单。
  next.links = make_memory_entity_link_resolver(
      next.aspect_priority, make_registry_builtin_order(next.registry));
  next.aggregate_priority = make_memory_aggregate_priority();
  next.router = make_router(next.registry, next.links, next.aspect_priority,
                            next.aggregate_priority);
  next.media_router = make_media_router(next.router);

  std::lock_guard<std::mutex> lock(mutex());
  state() = std::move(next);
}

bool media_runtime_initialized() noexcept {
  std::lock_guard<std::mutex> lock(mutex());
  return state().media_router != nullptr;
}

std::shared_ptr<MediaRouter> media_router_ptr() noexcept {
  std::lock_guard<std::mutex> lock(mutex());
  return state().media_router;
}

std::shared_ptr<LibraryRegistry> library_registry_ptr() noexcept {
  std::lock_guard<std::mutex> lock(mutex());
  return state().registry;
}

std::shared_ptr<EntityLinkResolver> entity_links_ptr() noexcept {
  std::lock_guard<std::mutex> lock(mutex());
  return state().links;
}

MediaRouter &media_router() {
  std::lock_guard<std::mutex> lock(mutex());
  if (state().media_router == nullptr) {
    throw std::runtime_error("media_runtime 未初始化");
  }
  return *state().media_router;
}

LibraryRegistry &library_registry() {
  std::lock_guard<std::mutex> lock(mutex());
  if (state().registry == nullptr) {
    throw std::runtime_error("media_runtime 未初始化");
  }
  return *state().registry;
}

EntityLinkResolver &entity_links() {
  std::lock_guard<std::mutex> lock(mutex());
  if (state().links == nullptr) {
    throw std::runtime_error("media_runtime 未初始化");
  }
  return *state().links;
}

void shutdown_media_runtime() noexcept {
  Runtime previous;
  {
    std::lock_guard<std::mutex> lock(mutex());
    previous = std::move(state());
    state() = Runtime{};
  }
  // 关闭动作放在锁外：close() 可能阻塞，也可能回调注册表。
  if (previous.registry != nullptr) {
    previous.registry->shutdown();
  }
}

} // namespace media_library
