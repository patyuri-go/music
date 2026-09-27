#ifndef JELLYFIN_IDS_H
#define JELLYFIN_IDS_H

#include <string>

namespace jellyfin {

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
