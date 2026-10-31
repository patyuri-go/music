#include "media_library/netease_library.h"

#include <utility>

namespace media_library {

NeteaseLibrary::NeteaseLibrary(
    std::string library_id, std::string display_name,
    std::shared_ptr<api_enhanced::NeteaseClient> client)
    : library_id_(std::move(library_id)),
      display_name_(std::move(display_name)), client_(std::move(client)) {}

void NeteaseLibrary::initialize() {
  auto self = shared_from_this();
  // N1 只有封面能力对象；它自己也显式返回 Unsupported，
  // 所以这里注册的意义是「能力表里有一项，而不是查不到」。
  register_capability<ArtworkBytes>(self);
}

LibraryIdentity NeteaseLibrary::identity() const {
  return LibraryIdentity{library_id_, display_name_, "netease"};
}

CapabilitySet NeteaseLibrary::capabilities() const {
  CapabilitySet set;
  // 11 项全部显式列出：缺项会被上层当成「没声明」而静默跳过，
  // 显式的 false 才能让 /api/v1/libraries 如实反映现状。
  const auto add = [&set](CapabilityId id, bool supported,
                          std::optional<ErrorCode> reason) {
    CapabilityState state;
    state.id = id;
    state.supported = supported;
    state.available = supported;
    state.reason = reason;
    set.items.push_back(std::move(state));
  };
  // N1 只做注册与健康探测，所有数据能力关着。
  add(CapabilityId::CatalogRead, false, ErrorCode::Unsupported);
  add(CapabilityId::Search, false, ErrorCode::Unsupported);
  add(CapabilityId::StreamResolve, false, ErrorCode::Unsupported);
  add(CapabilityId::LyricsRead, false, ErrorCode::Unsupported);
  add(CapabilityId::ArtworkRead, false, ErrorCode::Unsupported);
  add(CapabilityId::PlaylistRead, false, ErrorCode::Unsupported);
  add(CapabilityId::PlaylistWrite, false, ErrorCode::Unsupported);
  add(CapabilityId::FavoriteRead, false, ErrorCode::Unsupported);
  add(CapabilityId::FavoriteWrite, false, ErrorCode::Unsupported);
  add(CapabilityId::LibraryWrite, false, ErrorCode::Unsupported);
  add(CapabilityId::Auth, false, ErrorCode::Unsupported);
  return set;
}

void NeteaseLibrary::close() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  client_.reset();
}

Result<ArtworkLocation>
NeteaseLibrary::artwork_location(const ArtworkRef &ref, const CallContext &) {
  (void)ref;
  return Result<ArtworkLocation>::failure(
      Error::make(ErrorCode::Unsupported, "netease artwork is not wired yet"));
}

HealthSnapshot NeteaseLibrary::health() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return health_;
}

void NeteaseLibrary::probe_health(const CallContext &ctx) {
  HealthSnapshot health;
  std::shared_ptr<api_enhanced::NeteaseClient> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = client_;
  }

  // 探测失败不是构造失败：把库标成不可用后照常注册，列表里仍能看到它。
  if (client == nullptr) {
    health.state = HealthState::Unavailable;
    health.reason = ErrorCode::ProviderUnavailable;
  } else {
    auto status = client->login_status(ctx);
    if (!status.ok()) {
      health.state = HealthState::Unavailable;
      health.reason = status.error().code;
    } else if (status.value().authenticated) {
      health.state = HealthState::Ok;
    } else {
      // 匿名也能搜索、拿公开歌单、播免费曲，所以不是故障。
      health.state = HealthState::Unauthenticated;
      health.reason = ErrorCode::AuthRequired;
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  health_ = health;
}

Result<std::shared_ptr<NeteaseLibrary>>
make_netease_library(const std::string &library_id,
                     const std::string &display_name,
                     const api_enhanced::NeteaseTransportOptions &transport,
                     const api_enhanced::NeteaseClientOptions &client_options) {
  auto http = api_enhanced::make_http_transport(transport);
  if (http == nullptr) {
    return Result<std::shared_ptr<NeteaseLibrary>>::failure(
        Error::make(ErrorCode::InvalidRequest,
                    "netease api_base is not a valid http(s) url"));
  }

  auto client =
      std::make_shared<api_enhanced::NeteaseClient>(std::move(http),
                                                    client_options);
  auto library = std::make_shared<NeteaseLibrary>(library_id, display_name,
                                                  std::move(client));
  library->initialize();
  library->probe_health(CallContext{});

  return Result<std::shared_ptr<NeteaseLibrary>>::success(std::move(library));
}

} // namespace media_library
