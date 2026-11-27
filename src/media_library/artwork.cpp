#include "media_library/artwork.h"

#include "http_util.h"
#include "media_library/playback.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace media_library {
namespace {

namespace fs = std::filesystem;

bool is_http_url(const std::string &locator) {
  return locator.rfind("http://", 0) == 0 ||
         locator.rfind("https://", 0) == 0;
}

// 解析一次：走路由选源 -> 拿 PlaybackSource（locator + 打开策略）。
// locator 只在本函数返回值里活着，不会外泄。
struct Resolved {
  PlaybackSource source;
  std::string library_id;
};

Result<Resolved> resolve_location(const ArtworkRef &ref, const CallContext &ctx,
                                  MediaRouter &router) {
  Result<Sourced<PlaybackSource>> routed =
      router.resolve_artwork_source(ref.owner, ctx);
  if (!routed.ok()) {
    return Result<Resolved>::failure(routed.error());
  }

  Sourced<PlaybackSource> sourced = std::move(routed).value();
  Resolved resolved;
  resolved.source = std::move(sourced.value);
  resolved.library_id = sourced.origin.library_id;
  return Result<Resolved>::success(std::move(resolved));
}

std::string guess_mime(const PlaybackSource &source) {
  if (!source.mime_type.empty()) {
    return source.mime_type;
  }
  if (source.cache_virtual_path.has_value()) {
    return http_util::mimeType(fs::path(*source.cache_virtual_path));
  }
  if (!is_http_url(source.internal_locator)) {
    return http_util::mimeType(fs::path(source.internal_locator));
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

  Result<OpenedArtwork> open(const ArtworkRef &ref, const CallContext &ctx,
                             bool want_stream) override {
    Result<Resolved> resolved = resolve_location(ref, ctx, *router_);
    if (!resolved.ok()) {
      return Result<OpenedArtwork>::failure(resolved.error());
    }

    Resolved value = std::move(resolved).value();
    OpenedArtwork out;
    out.info.mime_type = guess_mime(value.source);
    out.info.size_bytes = value.source.content_length.value_or(-1);
    out.info.etag = value.source.cache_tag;
    out.info.library_id = value.library_id;

    if (want_stream) {
      // 缓存命中/生成由库层决定并用 kind=Cache 表达；这里只按 kind 建流。
      Result<std::unique_ptr<ByteStream>> stream =
          make_audio_stream(value.source);
      if (!stream.ok()) {
        return Result<OpenedArtwork>::failure(stream.error());
      }
      out.stream = std::move(stream).value();
    }
    return Result<OpenedArtwork>::success(std::move(out));
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
