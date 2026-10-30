#include "media_library/library_directory.h"

#include <utility>

namespace media_library {
namespace {

LibraryView view_of(const LibraryRegistration &registration,
                    const HealthSnapshot &health,
                    const std::optional<CapabilitySet> &capabilities) {
  LibraryView view;
  view.library_id = registration.library_id;
  view.display_name = registration.display_name;
  view.platform = registration.platform;
  view.priority = registration.priority;
  view.enabled = registration.enabled;
  view.health = health.state;
  view.health_reason = health.reason;
  view.health_checked_at = health.checked_at;
  if (capabilities.has_value()) {
    for (const CapabilityState &state : capabilities->items) {
      if (state.supported) {
        view.supported.push_back(state.id);
      }
      if (state.supported && state.available) {
        view.available.push_back(state.id);
      }
    }
  }
  return view;
}

} // namespace

LibraryDirectory::LibraryDirectory(std::shared_ptr<LibraryRegistry> registry)
    : registry_(std::move(registry)) {}

std::vector<LibraryView> LibraryDirectory::list() const {
  std::vector<LibraryView> out;
  if (registry_ == nullptr) {
    return out;
  }
  for (const LibraryRegistration &registration : registry_->list()) {
    out.push_back(view_of(registration, registry_->health(registration.library_id),
                          registry_->capabilities(registration.library_id)));
  }
  return out;
}

std::optional<LibraryView> LibraryDirectory::find(
    std::string_view library_id) const {
  if (registry_ == nullptr) {
    return std::nullopt;
  }
  const std::optional<LibraryRegistration> registration =
      registry_->find(library_id);
  if (!registration.has_value()) {
    return std::nullopt;
  }
  return view_of(*registration, registry_->health(registration->library_id),
                 registry_->capabilities(registration->library_id));
}

std::vector<LibraryView> LibraryDirectory::visible() const {
  std::vector<LibraryView> out;
  for (LibraryView &view : list()) {
    if (view.enabled && view.health != HealthState::Unavailable) {
      out.push_back(std::move(view));
    }
  }
  return out;
}

std::optional<CapabilitySet>
LibraryDirectory::capabilities(std::string_view library_id) const {
  if (registry_ == nullptr) {
    return std::nullopt;
  }
  return registry_->capabilities(library_id);
}

bool LibraryDirectory::supports(std::string_view library_id,
                                CapabilityId capability) const {
  const std::optional<CapabilitySet> set = capabilities(library_id);
  return set.has_value() && set->supports(capability);
}

HealthSnapshot LibraryDirectory::health(std::string_view library_id) const {
  if (registry_ == nullptr) {
    HealthSnapshot snapshot;
    snapshot.state = HealthState::Unavailable;
    snapshot.reason = ErrorCode::ProviderUnavailable;
    return snapshot;
  }
  return registry_->health(library_id);
}

std::shared_ptr<LibraryDirectory>
make_library_directory(std::shared_ptr<LibraryRegistry> registry) {
  return std::make_shared<LibraryDirectory>(std::move(registry));
}

} // namespace media_library
