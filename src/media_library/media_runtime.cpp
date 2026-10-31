#include "media_library/media_runtime.h"

#include "media_library/local_library.h"
#include "media_library/router.h"

#include <optional>

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
  std::shared_ptr<PlaybackService> playback;
  std::shared_ptr<ArtworkService> artwork;
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

void initialize_media_runtime(std::vector<LibrarySetup> libraries) {
  if (libraries.empty()) {
    throw std::invalid_argument(
        "initialize_media_runtime: no library to register");
  }

  Runtime next;
  next.registry = make_library_registry();

  for (LibrarySetup &setup : libraries) {
    if (setup.library == nullptr) {
      throw std::invalid_argument(
          "initialize_media_runtime: library is null");
    }
    if (setup.options.library_id.empty()) {
      throw std::invalid_argument(
          "initialize_media_runtime: library_id is empty");
    }

    LibraryRegistration registration;
    registration.library_id = setup.options.library_id;
    registration.display_name = setup.options.display_name.empty()
                                    ? setup.options.library_id
                                    : setup.options.display_name;
    registration.platform = setup.options.platform;
    registration.priority = setup.options.priority;
    registration.enabled = setup.options.enabled;
    registration.limits = setup.options.limits;
    registration.initial_health = setup.options.initial_health;
    registration.initial_health_reason = setup.options.initial_health_reason;

    const std::string library_id = registration.library_id;
    const Status added =
        next.registry->add(std::move(setup.library), registration);
    if (!added.ok()) {
      throw std::runtime_error(
          "initialize_media_runtime: " + library_id + ": " +
          std::string(error_code_name(added.error().code)));
    }
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
  next.playback =
      make_playback_service(next.media_router, make_system_clock());
  next.artwork = make_artwork_service(next.media_router, next.registry);

  std::lock_guard<std::mutex> lock(mutex());
  state() = std::move(next);
}

void initialize_media_runtime(std::shared_ptr<Library> library,
                              const MediaRuntimeOptions &options) {
  std::vector<LibrarySetup> libraries;
  libraries.push_back(LibrarySetup{std::move(library), options});
  initialize_media_runtime(std::move(libraries));
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

std::shared_ptr<PlaybackService> playback_service_ptr() noexcept {
  std::lock_guard<std::mutex> lock(mutex());
  return state().playback;
}

std::shared_ptr<ArtworkService> artwork_service_ptr() noexcept {
  std::lock_guard<std::mutex> lock(mutex());
  return state().artwork;
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

PlaybackService &playback_service() {
  std::lock_guard<std::mutex> lock(mutex());
  if (state().playback == nullptr) {
    throw std::runtime_error("media_runtime 未初始化");
  }
  return *state().playback;
}

ArtworkService &artwork_service() {
  std::lock_guard<std::mutex> lock(mutex());
  if (state().artwork == nullptr) {
    throw std::runtime_error("media_runtime 未初始化");
  }
  return *state().artwork;
}

namespace {

// TODO(N6): 跨库聚合完成后删除这些兼容访问器。
std::shared_ptr<LocalLibrary> local_library_impl() {
  std::shared_ptr<LibraryRegistry> registry = library_registry_ptr();
  if (registry == nullptr) {
    return nullptr;
  }
  for (const LibraryRegistration &entry : registry->list()) {
    if (entry.platform != PlatformKind::Local) {
      continue;
    }
    std::shared_ptr<Library> library = registry->library(entry.library_id);
    if (auto local = std::dynamic_pointer_cast<LocalLibrary>(library)) {
      return local;
    }
  }
  return nullptr;
}

} // namespace

std::optional<SourceRef> compat_ref_for(EntityType type, int internal_id) {
  std::shared_ptr<LocalLibrary> local = local_library_impl();
  if (local == nullptr) {
    return std::nullopt;
  }
  return local->ref_for_compat(type, internal_id);
}

std::optional<int> compat_internal_id_of(const SourceRef &ref) {
  std::shared_ptr<LocalLibrary> local = local_library_impl();
  if (local == nullptr) {
    return std::nullopt;
  }
  return local->internal_id_of(ref);
}

std::optional<TrackTransport> compat_track_transport(int internal_id) {
  std::shared_ptr<LocalLibrary> local = local_library_impl();
  if (local == nullptr) {
    return std::nullopt;
  }
  return local->track_transport(internal_id);
}

bool compat_snapshot(CatalogSnapshot &out) {
  std::shared_ptr<LocalLibrary> local = local_library_impl();
  if (local == nullptr) {
    return false;
  }
  out = local->snapshot();
  return true;
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
