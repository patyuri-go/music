#include "media_library/playlist_directory.h"

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace media_library {
namespace {

std::int64_t current_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 5.3.10：全部来源失败时的整体错误码优先级（数字越小越优先）。
// 与 router.cpp 的同名逻辑保持一致。
int aggregate_error_rank(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::PermissionDenied:
    return 0;
  case ErrorCode::AuthRequired:
    return 1;
  case ErrorCode::RateLimited:
    return 2;
  case ErrorCode::ProviderUnavailable:
    return 3;
  case ErrorCode::Timeout:
    return 4;
  default:
    return 5;
  }
}

std::vector<RouteAttempt> single_hop(const std::string &library_id,
                                     std::int64_t elapsed_ms) {
  RouteAttempt attempt;
  attempt.library_id = library_id;
  attempt.ok = true;
  attempt.elapsed_ms = elapsed_ms;
  return {std::move(attempt)};
}

// 汇总里的一条：来源就是歌单自带的库，不做换源，链只有一跳。
Sourced<Playlist> sourced_playlist(Playlist value,
                                  const PlaylistRef &ref) {
  Sourced<Playlist> output;
  output.origin.library_id = ref.library_id;
  output.origin.ref = ref;
  output.origin.aspect = std::nullopt;
  output.origin.substituted = false;
  output.origin.chain = single_hop(ref.library_id, 0);
  output.value = std::move(value);
  return output;
}

// ---------------------------------------------------------------------------
// 聚合游标。
//
// 各库自己的分页游标互不可比（远端是 offset，另一个库可能是别的语义），
// 所以总管签发自家的游标：按 candidates() 顺序给每个库记一段进度。
// 格式：pl1|<library_id>=<offset>|<library_id>=-
//   <offset>  该库下一段的起点（十进制，库自己要的游标）
//   -         该库本次遍历已取完，后续页跳过
// 空串或解析失败一律当作「从头开始」，避免脏游标把分页卡死。
// library_id 由配置给出（形如 local-main / netease-main），不含 '|' 与 '='。
constexpr char kCursorPrefix[] = "pl1";

struct LibraryCursor {
  std::string library_id;
  std::int64_t offset = 0;
  bool done = false;
};

std::string encode_cursor(const std::vector<LibraryCursor> &state) {
  std::string out(kCursorPrefix);
  for (const LibraryCursor &entry : state) {
    out += '|';
    out += entry.library_id;
    out += '=';
    out += entry.done ? std::string("-") : std::to_string(entry.offset);
  }
  return out;
}

std::vector<LibraryCursor> decode_cursor(const std::optional<std::string> &text) {
  std::vector<LibraryCursor> out;
  if (!text.has_value() || text->empty()) {
    return out;
  }
  const std::string &raw = *text;
  const std::string prefix = std::string(kCursorPrefix) + "|";
  if (raw.compare(0, prefix.size(), prefix) != 0) {
    return out;
  }
  std::size_t pos = prefix.size();
  while (pos < raw.size()) {
    const std::size_t bar = raw.find('|', pos);
    const std::string segment = raw.substr(
        pos, bar == std::string::npos ? std::string::npos : bar - pos);
    const std::size_t eq = segment.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= segment.size()) {
      return {};
    }
    LibraryCursor entry;
    entry.library_id = segment.substr(0, eq);
    const std::string value = segment.substr(eq + 1);
    if (value == "-") {
      entry.done = true;
    } else {
      try {
        const long long parsed = std::stoll(value);
        if (parsed < 0) {
          return {};
        }
        entry.offset = static_cast<std::int64_t>(parsed);
      } catch (const std::exception &) {
        return {};
      }
    }
    out.push_back(std::move(entry));
    if (bar == std::string::npos) {
      break;
    }
    pos = bar + 1;
  }
  return out;
}

} // namespace

PlaylistDirectory::PlaylistDirectory(std::shared_ptr<LibraryRegistry> registry)
    : registry_(std::move(registry)) {}

Result<RoutedPage<Playlist>>
PlaylistDirectory::list_playlists(const PageRequest &page,
                                 const CallContext &ctx) {
  RoutedPage<Playlist> output;

  // 没有注册表或没有库声明 playlist_read 都不是错误：就是一个空目录。
  if (registry_ == nullptr) {
    return Result<RoutedPage<Playlist>>::success(std::move(output));
  }
  const std::vector<LibraryRegistration> candidates =
      registry_->candidates(CapabilityId::PlaylistRead, true);
  if (candidates.empty()) {
    return Result<RoutedPage<Playlist>>::success(std::move(output));
  }

  const int limit = page.limit > 0 ? page.limit : 50;

  // 进度表按候选顺序初始化：游标里有的沿用，没有的从 0 开始。
  const std::vector<LibraryCursor> resumed = decode_cursor(page.cursor);
  std::vector<LibraryCursor> state;
  state.reserve(candidates.size());
  for (const LibraryRegistration &registration : candidates) {
    LibraryCursor entry;
    entry.library_id = registration.library_id;
    for (const LibraryCursor &previous : resumed) {
      if (previous.library_id == entry.library_id) {
        entry.offset = previous.offset;
        entry.done = previous.done;
        break;
      }
    }
    state.push_back(std::move(entry));
  }

  std::vector<Error> failures;
  bool any_success = false;
  int remaining = limit;

  for (std::size_t i = 0; i < state.size() && remaining > 0; ++i) {
    LibraryCursor &entry = state[i];
    if (entry.done) {
      continue;
    }

    SourceStatus status;
    status.library_id = entry.library_id;
    const std::int64_t started = current_ms();

    // 本轮内该库一旦失败或被跳过就不再重试：否则「没有任何结果」时
    // has_more 永远为真，客户端会在同一页上无限翻下去。
    // 下一次从头（游标为空）的请求会重新尝试它。
    auto lease = registry_->acquire(entry.library_id,
                                    CapabilityId::PlaylistRead, current_ms());
    if (!lease.ok()) {
      entry.done = true;
      status.state = source_state_from(lease.error().code);
      status.error = lease.error();
      status.elapsed_ms = current_ms() - started;
      output.sources.push_back(std::move(status));
      failures.push_back(lease.error());
      continue;
    }

    auto library = lease.value().library();
    auto reader = library->capability<PlaylistReader>();
    if (!reader.has_value()) {
      Error error = Error::make(ErrorCode::Unsupported, "no playlist reader");
      error.library_id = entry.library_id;
      lease.value().finish(error);
      entry.done = true;
      status.state = source_state_from(error.code);
      status.error = error;
      status.elapsed_ms = current_ms() - started;
      output.sources.push_back(std::move(status));
      failures.push_back(std::move(error));
      continue;
    }

    // 只向该库要「本页还缺的条数」，起点交给它自己的游标语义。
    const PageRequest slice{std::to_string(entry.offset), remaining};
    Result<Page<Playlist>> result = reader->get().list_playlists(slice, ctx);
    status.elapsed_ms = current_ms() - started;

    if (!result.ok()) {
      lease.value().finish(result.error());
      entry.done = true;
      status.state = source_state_from(result.error().code);
      status.error = result.error();
      output.sources.push_back(std::move(status));
      failures.push_back(result.error());
      continue;
    }

    lease.value().finish(std::nullopt);
    status.state = SourceState::Ok;
    status.item_count = static_cast<int>(result.value().items.size());
    output.sources.push_back(std::move(status));
    any_success = true;

    const std::size_t taken = result.value().items.size();
    for (Playlist &playlist : result.value().items) {
      const PlaylistRef ref = playlist.ref;
      output.items.push_back(sourced_playlist(std::move(playlist), ref));
    }
    entry.offset += static_cast<std::int64_t>(taken);
    remaining -= static_cast<int>(taken);

    // 该库自己说没了，或者这次一条都没给（再问也是同样的空页，判完）。
    if (taken == 0 || !result.value().has_more) {
      entry.done = true;
    }
  }

  // 全部来源都失败且没有任何结果：按优先级给一个整体错误（5.3.10）。
  if (!any_success && output.items.empty() && !failures.empty()) {
    std::size_t chosen = 0;
    for (std::size_t i = 1; i < failures.size(); ++i) {
      if (aggregate_error_rank(failures[i].code) <
          aggregate_error_rank(failures[chosen].code)) {
        chosen = i;
      }
    }
    return Result<RoutedPage<Playlist>>::failure(failures[chosen]);
  }

  bool more = false;
  for (const LibraryCursor &entry : state) {
    if (!entry.done) {
      more = true;
      break;
    }
  }
  output.has_more = more;
  if (more) {
    output.next_cursor = encode_cursor(state);
  }
  return Result<RoutedPage<Playlist>>::success(std::move(output));
}

Result<Sourced<Playlist>>
PlaylistDirectory::get_playlist(const PlaylistRef &ref,
                               const CallContext &ctx) {
  if (ref.entity_type != EntityType::Playlist) {
    return Result<Sourced<Playlist>>::failure(
        Error::make(ErrorCode::InvalidRequest, "invalid playlist reference"));
  }
  if (registry_ == nullptr) {
    return Result<Sourced<Playlist>>::failure(Error::make(
        ErrorCode::ProviderUnavailable, "library registry is not configured"));
  }

  auto lease = registry_->acquire(ref.library_id, CapabilityId::PlaylistRead,
                                  current_ms());
  if (!lease.ok()) {
    return Result<Sourced<Playlist>>::failure(lease.error());
  }

  auto library = lease.value().library();
  auto reader = library->capability<PlaylistReader>();
  if (!reader.has_value()) {
    Error error = Error::make(ErrorCode::Unsupported, "no playlist reader");
    error.library_id = ref.library_id;
    lease.value().finish(error);
    return Result<Sourced<Playlist>>::failure(std::move(error));
  }

  Result<Playlist> result = reader->get().get_playlist(ref, ctx);
  if (!result.ok()) {
    lease.value().finish(result.error());
    return Result<Sourced<Playlist>>::failure(result.error());
  }
  lease.value().finish(std::nullopt);

  const PlaylistRef source = result.value().ref;
  return Result<Sourced<Playlist>>::success(
      sourced_playlist(std::move(result.value()), source));
}

std::shared_ptr<PlaylistDirectory>
make_playlist_directory(std::shared_ptr<LibraryRegistry> registry) {
  return std::make_shared<PlaylistDirectory>(std::move(registry));
}

} // namespace media_library
