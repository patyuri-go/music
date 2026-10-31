#include "media_library/artwork.h"

#include "http_util.h"
#include "image_cache.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace media_library {
namespace {

namespace fs = std::filesystem;

Error internal_error(std::string message) {
  return Error::make(ErrorCode::Internal, std::move(message));
}

bool is_http_url(const std::string &locator) {
  return locator.rfind("http://", 0) == 0 ||
         locator.rfind("https://", 0) == 0;
}

// 解析一次：走路由选源 -> 找库 -> 拿 ArtworkBytes -> 得 locator。
// locator 只在本函数返回值里活着，不会外泄。
struct Resolved {
  ArtworkLocation location;
  std::string library_id;
  std::string etag;
};

Result<Resolved> resolve_location(const ArtworkRef &ref, const CallContext &ctx,
                                  MediaRouter &router,
                                  LibraryRegistry &registry) {
  Result<Sourced<ArtworkRef>> routed = router.resolve_artwork(ref.owner, ctx);
  if (!routed.ok()) {
    return Result<Resolved>::failure(routed.error());
  }

  Sourced<ArtworkRef> sourced = std::move(routed).value();
  std::shared_ptr<Library> library = registry.library(sourced.origin.library_id);
  if (library == nullptr) {
    return Result<Resolved>::failure(internal_error(
        "artwork origin library is not registered: " + sourced.origin.library_id));
  }

  auto capability = library->capability<ArtworkBytes>();
  if (!capability.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported,
                              "library does not expose artwork bytes");
    error.library_id = sourced.origin.library_id;
    error.capability = std::string(capability_name(CapabilityId::ArtworkRead));
    return Result<Resolved>::failure(std::move(error));
  }

  Result<ArtworkLocation> located =
      capability->get().artwork_location(sourced.value, ctx);
  if (!located.ok()) {
    return Result<Resolved>::failure(located.error());
  }

  Resolved resolved;
  resolved.location = std::move(located).value();
  resolved.library_id = sourced.origin.library_id;
  resolved.etag = sourced.value.cache_tag;
  return Result<Resolved>::success(std::move(resolved));
}

std::string guess_mime(const ArtworkLocation &location) {
  if (!location.mime_type.empty()) {
    return location.mime_type;
  }
  if (location.cache_virtual_path.has_value()) {
    return http_util::mimeType(fs::path(*location.cache_virtual_path));
  }
  if (!is_http_url(location.locator)) {
    return http_util::mimeType(fs::path(location.locator));
  }
  return {};
}

class ArtworkServiceImpl final : public ArtworkService {
public:
  ArtworkServiceImpl(std::shared_ptr<MediaRouter> router,
                     std::shared_ptr<LibraryRegistry> registry,
                     ArtworkServiceOptions options)
      : router_(std::move(router)), registry_(std::move(registry)),
        options_(options) {
    if (router_ == nullptr) {
      throw std::invalid_argument("make_artwork_service: router is null");
    }
    if (registry_ == nullptr) {
      throw std::invalid_argument("make_artwork_service: registry is null");
    }
  }

  Result<ArtworkInfo> info(const ArtworkRef &ref,
                           const CallContext &ctx) override {
    Result<Resolved> resolved = resolve_location(ref, ctx, *router_, *registry_);
    if (!resolved.ok()) {
      return Result<ArtworkInfo>::failure(resolved.error());
    }

    Resolved value = std::move(resolved).value();
    ArtworkInfo result;
    result.mime_type = guess_mime(value.location);
    result.size_bytes = value.location.size_bytes.value_or(-1);
    result.etag = value.etag;
    result.library_id = value.library_id;
    return Result<ArtworkInfo>::success(std::move(result));
  }

  Result<std::unique_ptr<ByteStream>>
  open(const ArtworkRef &ref, const CallContext &ctx) override {
    Result<Resolved> resolved = resolve_location(ref, ctx, *router_, *registry_);
    if (!resolved.ok()) {
      return Result<std::unique_ptr<ByteStream>>::failure(resolved.error());
    }

    Resolved value = std::move(resolved).value();
    const std::string mime = guess_mime(value.location);

    // 本地库：优先让 image_cache 给出缩放/转码产物；命中就是文件流。
    if (value.location.cache_virtual_path.has_value()) {
      if (auto cache = image_cache::get()) {
        std::string error;
        if (const auto hit = cache->ensure(*value.location.cache_virtual_path,
                                          "original", error)) {
          return make_file_stream(hit->string(), mime);
        }
        CROW_LOG_DEBUG << "[artwork] image_cache ensure failed for "
                       << *value.location.cache_virtual_path << ": " << error;
      }
    }

    if (is_http_url(value.location.locator)) {
      return make_http_stream(value.location.locator, mime,
                              value.location.size_bytes, true);
    }
    return make_file_stream(value.location.locator, mime);
  }

private:
  std::shared_ptr<MediaRouter> router_;
  std::shared_ptr<LibraryRegistry> registry_;
  ArtworkServiceOptions options_;
};

} // namespace

std::shared_ptr<ArtworkService>
make_artwork_service(std::shared_ptr<MediaRouter> router,
                     std::shared_ptr<LibraryRegistry> registry,
                     const ArtworkServiceOptions &options) {
  return std::make_shared<ArtworkServiceImpl>(std::move(router),
                                              std::move(registry), options);
}

} // namespace media_library
