#include "media_library/library_runtime.h"

#include <mutex>
#include <stdexcept>

namespace media_library {
namespace {

std::mutex &runtime_mutex() {
  static std::mutex instance;
  return instance;
}

std::shared_ptr<LocalLibrary> &runtime_holder() {
  static std::shared_ptr<LocalLibrary> instance;
  return instance;
}

} // namespace

void initialize_library(std::string library_id, std::string display_name,
                        std::string db_path) {
  auto created = std::make_shared<LocalLibrary>(std::move(library_id),
                                               std::move(display_name),
                                               std::move(db_path));
  created->initialize();

  std::lock_guard<std::mutex> lock(runtime_mutex());
  runtime_holder() = std::move(created);
}

bool library_initialized() noexcept {
  std::lock_guard<std::mutex> lock(runtime_mutex());
  return runtime_holder() != nullptr;
}

std::shared_ptr<LocalLibrary> library_ptr() noexcept {
  std::lock_guard<std::mutex> lock(runtime_mutex());
  return runtime_holder();
}

LocalLibrary &library() {
  const auto instance = library_ptr();
  if (!instance) {
    throw std::runtime_error("media_library 未初始化，请先调用 initialize_library");
  }
  return *instance;
}

void shutdown_library() noexcept {
  std::shared_ptr<LocalLibrary> released;
  {
    std::lock_guard<std::mutex> lock(runtime_mutex());
    released = std::move(runtime_holder());
  }
  if (released) {
    released->close();
  }
}

} // namespace media_library
