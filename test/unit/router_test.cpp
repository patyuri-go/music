// 多来源路由（media_library::Router）的纯单元测试。
//
// 只压 Aspect::Stream：其他方面要连带实现 CatalogReader / LyricsProvider
// 等整套接口，和本层关心的「候选生成 + 优先级 + 回退」没有关系。
//
// 不读 config.json、不连数据库、不访问网络：registry 用内存实现，
// EntityLinkResolver 与 AspectPriority 用下面的假实现。

#include "media_library/error.h"
#include "media_library/library.h"
#include "media_library/registry.h"
#include "media_library/router.h"

#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using media_library::AccessMode;
using media_library::Aspect;
using media_library::AspectPriority;
using media_library::CallContext;
using media_library::capability_name;
using media_library::CapabilityId;
using media_library::CapabilitySet;
using media_library::CapabilityState;
using media_library::EntityLinkResolver;
using media_library::EntityLinkSummary;
using media_library::EntityLinkView;
using media_library::EntityType;
using media_library::Error;
using media_library::ErrorCode;
using media_library::Library;
using media_library::LibraryIdentity;
using media_library::LibraryRegistration;
using media_library::LibraryRegistry;
using media_library::LinkOverrides;
using media_library::LinkedRef;
using media_library::make_library_registry;
using media_library::make_memory_aggregate_priority;
using media_library::make_router;
using media_library::PlaybackKind;
using media_library::PlaybackSource;
using media_library::Result;
using media_library::Router;
using media_library::SourceRef;
using media_library::Status;
using media_library::StreamResolver;
using media_library::TrackRef;

// ---- 假实现 ---------------------------------------------------------------

// 结果由 std::function 决定，测试里逐例替换；同时记录调用过的 library_id。
class FakeStreamResolver final : public StreamResolver {
public:
  std::function<Result<PlaybackSource>(const TrackRef &)> handler;
  std::vector<std::string> calls;

  Result<PlaybackSource> resolve_playback(const TrackRef &ref,
                                          const CallContext &) override {
    calls.push_back(ref.library_id);
    if (!handler) {
      return Result<PlaybackSource>::failure(
          Error::make(ErrorCode::NotFound, "no handler"));
    }
    return handler(ref);
  }
};

class FakeLibrary final : public Library {
public:
  FakeLibrary(std::string id, std::vector<CapabilityId> supported)
      : id_(std::move(id)) {
    for (const CapabilityId id : supported) {
      CapabilityState state;
      state.id = id;
      state.supported = true;
      state.available = true;
      caps_.items.push_back(state);
    }
    resolver_ = std::make_shared<FakeStreamResolver>();
    register_capability<StreamResolver>(resolver_);
  }

  LibraryIdentity identity() const override {
    return LibraryIdentity{id_, id_, "local"};
  }
  CapabilitySet capabilities() const override { return caps_; }
  void close() noexcept override {}

  FakeStreamResolver &resolver() { return *resolver_; }

private:
  std::string id_;
  CapabilitySet caps_;
  std::shared_ptr<FakeStreamResolver> resolver_;
};

// 默认「有链接」：成员与排序都由测试填。has_link=false 模拟查不到映射，
// 路由应退化为单源。
class FakeLinks final : public EntityLinkResolver {
public:
  bool has_link = true;
  std::vector<SourceRef> members;
  std::vector<std::string> order;

  Result<EntityLinkView> find_link(const SourceRef &ref,
                                   const CallContext &) const override {
    if (!has_link) {
      return Result<EntityLinkView>::failure(
          Error::make(ErrorCode::NotFound, "no link"));
    }
    EntityLinkView view;
    view.entity_type = ref.entity_type;
    for (const SourceRef &member : members) {
      view.refs.push_back(LinkedRef{member, "exact", true});
    }
    return Result<EntityLinkView>::success(std::move(view));
  }

  Result<std::vector<LinkedRef>> refs_of(const SourceRef &,
                                         const CallContext &) const override {
    std::vector<LinkedRef> out;
    for (const SourceRef &member : members) {
      out.push_back(LinkedRef{member, "exact", true});
    }
    return Result<std::vector<LinkedRef>>::success(std::move(out));
  }

  Result<LinkOverrides> overrides_of(const SourceRef &,
                                     const CallContext &) const override {
    return Result<LinkOverrides>::success(LinkOverrides{});
  }

  Result<std::vector<EntityLinkSummary>>
  list_links(std::optional<EntityType>, int, int,
             const CallContext &) const override {
    return Result<std::vector<EntityLinkSummary>>::success({});
  }

  Status link(const SourceRef &, const SourceRef &,
              const CallContext &) override {
    return Status::success();
  }
  Status remove_ref(const SourceRef &, const CallContext &) override {
    return Status::success();
  }
  Status delete_link(const SourceRef &, const CallContext &) override {
    return Status::success();
  }
  Status set_overrides(const SourceRef &, const LinkOverrides &,
                       const CallContext &) override {
    return Status::success();
  }
  Status clear_overrides(const SourceRef &, Aspect,
                         const CallContext &) override {
    return Status::success();
  }

  std::vector<std::string> effective_order(const SourceRef &,
                                           Aspect) const override {
    return order;
  }
};

// AspectPriority 用不到：候选顺序由链接的 effective_order 决定。
class FakeAspectPriority final : public AspectPriority {
public:
  std::vector<std::string> order(Aspect) const override { return {}; }
  Status set_order(Aspect, std::vector<std::string>) override {
    return Status::success();
  }
};

// ---- 装配辅助 -------------------------------------------------------------

TrackRef trackRef(const std::string &library_id, const std::string &entity_id) {
  return SourceRef{library_id, EntityType::Track, entity_id};
}

Result<PlaybackSource> playable(const TrackRef &ref) {
  PlaybackSource source;
  source.track = ref;
  source.kind = PlaybackKind::RemoteStream;
  source.mime_type = "audio/wav";
  source.access_mode = AccessMode::Proxy;
  source.internal_locator = "test://" + ref.library_id;
  return Result<PlaybackSource>::success(std::move(source));
}

Result<PlaybackSource> failWith(ErrorCode code) {
  return Result<PlaybackSource>::failure(Error::make(code, "test failure"));
}

std::shared_ptr<FakeLibrary> addLibrary(
    const std::shared_ptr<LibraryRegistry> &registry, const std::string &id,
    bool enabled = true,
    const std::vector<CapabilityId> &supported = {CapabilityId::StreamResolve}) {
  auto library = std::make_shared<FakeLibrary>(id, supported);
  LibraryRegistration registration;
  registration.library_id = id;
  registration.display_name = id;
  registration.enabled = enabled;
  EXPECT_TRUE(registry->add(library, registration).ok()) << "register " << id;
  return library;
}

struct Env {
  std::shared_ptr<LibraryRegistry> registry = make_library_registry();
  std::shared_ptr<FakeLinks> links = std::make_shared<FakeLinks>();
  std::shared_ptr<Router> router;

  void build() {
    router = make_router(registry, links, std::make_shared<FakeAspectPriority>(),
                         make_memory_aggregate_priority());
  }
};

// ---- 用例 -----------------------------------------------------------------

// 1. 查不到映射时退化为单源，直接用调用方给的引用。
TEST(RouterTest, NoLinkDegradesToSingleSource) {
  Env env;
  addLibrary(env.registry, "local-main");
  env.links->has_link = false;
  env.build();

  const TrackRef ref = trackRef("local-main", "10");
  const std::vector<std::string> order = env.router->order_of(ref, Aspect::Stream);
  ASSERT_EQ(order.size(), 1u);
  EXPECT_EQ(order.front(), "local-main");

  const auto resolved = env.router->resolve(ref, Aspect::Stream, CallContext{});
  ASSERT_TRUE(resolved.ok());
  EXPECT_EQ(resolved.value().library_id, "local-main");
  EXPECT_EQ(resolved.value().entity_type, EntityType::Track);
  EXPECT_EQ(resolved.value().entity_id, "10");
}

// 2. 有链接时按 effective_order 排序，而不是按成员加入顺序。
TEST(RouterTest, LinkOrderFollowsEffectiveOrder) {
  Env env;
  addLibrary(env.registry, "local-main");
  addLibrary(env.registry, "netease-main");
  env.links->members = {trackRef("local-main", "10"),
                        trackRef("netease-main", "n-10")};
  env.links->order = {"netease-main", "local-main"};
  env.build();

  const TrackRef ref = trackRef("local-main", "10");
  const auto resolved = env.router->resolve(ref, Aspect::Stream, CallContext{});
  ASSERT_TRUE(resolved.ok());
  EXPECT_EQ(resolved.value().library_id, "netease-main");
  EXPECT_EQ(resolved.value().entity_id, "n-10");
}

// 3. 只有同 entity_type 的成员进同一个候选池。
TEST(RouterTest, CandidatesFilterByEntityType) {
  Env env;
  addLibrary(env.registry, "local-main");
  addLibrary(env.registry, "netease-main");
  env.links->members = {trackRef("local-main", "10"), trackRef("netease-main", "n-10"),
                        SourceRef{"netease-main", EntityType::Album, "b-1"}};
  env.links->order = {"netease-main", "local-main"};
  env.build();

  const auto resolved =
      env.router->resolve(trackRef("local-main", "10"), Aspect::Stream, CallContext{});
  ASSERT_TRUE(resolved.ok());
  EXPECT_EQ(resolved.value().entity_type, EntityType::Track);
  EXPECT_EQ(resolved.value().library_id, "netease-main");
}

// 4. Stream 只接受 Track：拿 Album 引用取流时没有候选。
TEST(RouterTest, StreamAspectRejectsAlbumReferences) {
  Env env;
  addLibrary(env.registry, "local-main");
  env.links->members = {SourceRef{"local-main", EntityType::Album, "2"}};
  env.links->order = {"local-main"};
  env.build();

  const auto result = env.router->resolve_playback(
      SourceRef{"local-main", EntityType::Album, "2"}, CallContext{});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, ErrorCode::Unsupported);
}

// 5. 硬过滤：被禁用的库、没声明 StreamResolve 能力的库都不进候选。
TEST(RouterTest, DisabledAndUnsupportedLibrariesAreFiltered) {
  Env env;
  const auto local = addLibrary(env.registry, "local-main");
  local->resolver().handler = playable;
  addLibrary(env.registry, "netease-main", /*enabled=*/false);
  addLibrary(env.registry, "other-main", /*enabled=*/true, {CapabilityId::CatalogRead});
  env.links->members = {trackRef("local-main", "10"),
                        trackRef("netease-main", "n-10"),
                        trackRef("other-main", "o-10")};
  env.links->order = {"netease-main", "other-main", "local-main"};
  env.build();

  const auto result =
      env.router->resolve_playback(trackRef("local-main", "10"), CallContext{});
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().source.library_id, "local-main");
  // 只有一个可用候选，所以没有发生替换，也没有失败尝试。
  EXPECT_FALSE(result.value().substituted);
  ASSERT_EQ(result.value().attempts.size(), 1u);
}

// 6. 上游不可用会回退到下一个候选，并标记 substituted。
TEST(RouterTest, ProviderUnavailableFallsBackToNextCandidate) {
  Env env;
  const auto local = addLibrary(env.registry, "local-main");
  const auto remote = addLibrary(env.registry, "netease-main");
  local->resolver().handler = [](const TrackRef &) {
    return failWith(ErrorCode::ProviderUnavailable);
  };
  remote->resolver().handler = playable;
  env.links->members = {trackRef("local-main", "10"),
                        trackRef("netease-main", "n-10")};
  env.links->order = {"local-main", "netease-main"};
  env.build();

  const auto result =
      env.router->resolve_playback(trackRef("local-main", "10"), CallContext{});
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().source.library_id, "netease-main");
  EXPECT_TRUE(result.value().substituted);
  ASSERT_EQ(result.value().attempts.size(), 2u);
  EXPECT_FALSE(result.value().attempts[0].ok);
  EXPECT_EQ(result.value().attempts[0].library_id, "local-main");
  ASSERT_TRUE(result.value().attempts[0].error.has_value());
  EXPECT_EQ(*result.value().attempts[0].error, ErrorCode::ProviderUnavailable);
  EXPECT_TRUE(result.value().attempts[1].ok);
  EXPECT_EQ(local->resolver().calls.size(), 1u);
  EXPECT_EQ(remote->resolver().calls.size(), 1u);
}

// 7. 权限错误不回退：换库也解决不了，直接失败。
TEST(RouterTest, PermissionDeniedDoesNotFallBack) {
  Env env;
  const auto local = addLibrary(env.registry, "local-main");
  const auto remote = addLibrary(env.registry, "netease-main");
  local->resolver().handler = [](const TrackRef &) {
    return failWith(ErrorCode::PermissionDenied);
  };
  remote->resolver().handler = playable;
  env.links->members = {trackRef("local-main", "10"),
                        trackRef("netease-main", "n-10")};
  env.links->order = {"local-main", "netease-main"};
  env.build();

  const auto result =
      env.router->resolve_playback(trackRef("local-main", "10"), CallContext{});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, ErrorCode::PermissionDenied);
  EXPECT_EQ(remote->resolver().calls.size(), 0u);
}

// 8. NotFound 会回退（跨库补齐既有语义，改动 should_fall_back 必须动这条）。
TEST(RouterTest, NotFoundFallsBackToNextCandidate) {
  Env env;
  const auto local = addLibrary(env.registry, "local-main");
  const auto remote = addLibrary(env.registry, "netease-main");
  local->resolver().handler = [](const TrackRef &) {
    return failWith(ErrorCode::NotFound);
  };
  remote->resolver().handler = playable;
  env.links->members = {trackRef("local-main", "10"),
                        trackRef("netease-main", "n-10")};
  env.links->order = {"local-main", "netease-main"};
  env.build();

  const auto result =
      env.router->resolve_playback(trackRef("local-main", "10"), CallContext{});
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().source.library_id, "netease-main");
  EXPECT_TRUE(result.value().substituted);
  ASSERT_EQ(result.value().attempts.size(), 2u);
}

// 9. 候选为空时报 Unsupported，并把 library_id / capability 填全，
//    便于上层日志定位是哪个库缺哪个能力。
TEST(RouterTest, EmptyCandidatesReportUnsupportedWithContext) {
  Env env;
  addLibrary(env.registry, "local-main");
  env.links->members = {};  // 有链接视图，但没有同类型成员
  env.links->order = {"local-main"};
  env.build();

  const auto result =
      env.router->resolve_playback(trackRef("local-main", "10"), CallContext{});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, ErrorCode::Unsupported);
  EXPECT_EQ(result.error().library_id, "local-main");
  EXPECT_EQ(result.error().capability, capability_name(CapabilityId::StreamResolve));
}

// 10. order_of：无链接时返回自身库，有链接时返回链接的优先级。
TEST(RouterTest, OrderOfReflectsLinkOverrides) {
  Env env;
  addLibrary(env.registry, "local-main");
  addLibrary(env.registry, "netease-main");
  env.build();

  const TrackRef ref = trackRef("local-main", "10");
  env.links->has_link = false;
  const std::vector<std::string> single = env.router->order_of(ref, Aspect::Stream);
  ASSERT_EQ(single.size(), 1u);
  EXPECT_EQ(single.front(), "local-main");

  env.links->has_link = true;
  env.links->members = {ref, trackRef("netease-main", "n-10")};
  env.links->order = {"netease-main", "local-main"};
  const std::vector<std::string> linked =
      env.router->order_of(ref, Aspect::Stream);
  ASSERT_EQ(linked.size(), 2u);
  EXPECT_EQ(linked[0], "netease-main");
  EXPECT_EQ(linked[1], "local-main");
}

// 11. 候选最多 3 个：第 4 个库不会被尝试，即使它能出流。
TEST(RouterTest, CandidatesAreCappedAtThree) {
  Env env;
  std::vector<std::shared_ptr<FakeLibrary>> libraries;
  for (const std::string &id : {"lib-a", "lib-b", "lib-c", "lib-d"}) {
    auto library = addLibrary(env.registry, id);
    library->resolver().handler = [](const TrackRef &) {
      return failWith(ErrorCode::ProviderUnavailable);
    };
    libraries.push_back(std::move(library));
  }
  // 第 4 个库本来能出流；候选被截断，所以它不该被调用。
  libraries[3]->resolver().handler = playable;
  env.links->members = {trackRef("lib-a", "1"), trackRef("lib-b", "2"),
                        trackRef("lib-c", "3"), trackRef("lib-d", "4")};
  env.links->order = {"lib-a", "lib-b", "lib-c", "lib-d"};
  env.build();

  const auto result =
      env.router->resolve_playback(trackRef("lib-a", "1"), CallContext{});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, ErrorCode::ProviderUnavailable);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(libraries[i]->resolver().calls.size(), 1u) << "lib " << i;
  }
  EXPECT_EQ(libraries[3]->resolver().calls.size(), 0u);
}

} // namespace