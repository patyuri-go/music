#ifndef JELLYFIN_IDS_H
#define JELLYFIN_IDS_H

#include "media_library/source_ref.h"

#include <optional>
#include <string>
#include <string_view>

namespace jellyfin {

// ---------------------------------------------------------------------------
// 目录实体（曲目 / 专辑 / 艺术家）的对外 id
//
// 直接用 SourceRef 的规范字符串（`library_id:type:entity_id`）。entity_id 由
// EntityStore 分配后永不变更、不回收，因此天然是稳定 id；且不依赖任何库的
// 内部主键，远端 provider 也能给出可用的 id。
//
// 注意：`library_id` 与 `entity_id` 都会做百分号编码，而 `entity_id` 本身是
// 十六进制，实际不会出现 '%'。客户端不得解析这个字符串，只能原样回传。
// ---------------------------------------------------------------------------
std::string itemId(const media_library::SourceRef &ref);

// 解析目录实体 id；不是合法 SourceRef 时返回 nullopt。
// 调用方仍需校验 entity_type 是否符合预期端点。
std::optional<media_library::SourceRef> parseItemId(std::string_view text);

// ---------------------------------------------------------------------------
// 非目录实体（视图 / 流派）与遗留歌单条目的 id
//
// 保留旧形态：8 位类型码 + 24 位整数。这些不是 Library 目录实体，没有
// SourceRef 可用；流派与视图的整数由进程内快照分配。
// ---------------------------------------------------------------------------

// 实体类型编码：与数据库自增 id 组合成 32 位十六进制 GUID。
constexpr int kKindTrack = 1;
constexpr int kKindAlbum = 2;
constexpr int kKindArtist = 3;
constexpr int kKindGenre = 4;
constexpr int kKindView = 5;
constexpr int kKindPlaylist = 6;

// 生成 32 位十六进制 GUID：8 位类型码 + 24 位实体 id。
std::string makeId(int kind, int id);
// 解析 GUID；成功返回 true 并写出 kind/id。
bool parseId(const std::string &guid, int &kind, int &id);

const std::string &serverId();
const std::string &userId();
const std::string &userName();
const std::string &serverName();
void setServerName(const std::string &name);

// 固定的“音乐”视图 id。
std::string musicViewId();

// 固定的“歌单”视图 id。
std::string playlistsViewId();

} // namespace jellyfin

#endif
