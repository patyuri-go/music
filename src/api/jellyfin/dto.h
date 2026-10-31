#ifndef JELLYFIN_DTO_H
#define JELLYFIN_DTO_H

#include "crow/json.h"
#include "media_library/domain.h"

#include <string>
#include <utility>
#include <vector>

namespace jellyfin {

// 路由只持有统一领域模型，compat 层负责映射成 Jellyfin DTO。
// 这里的 id 已是稳定的对外 id（`to_string(SourceRef)`），由调用方通过
// `jellyfin::itemId()` 生成；空串表示「没有该父级」。
crow::json::wvalue trackItem(
    const media_library::Track &t, const std::string &trackId,
    const std::string &albumId, const std::string &albumName,
    const std::string &filePath,
    const std::vector<std::pair<std::string, std::string>> &trackArtists,
    const std::vector<std::pair<std::string, std::string>> &albumArtists,
    const std::vector<std::pair<std::string, int>> &genres);

crow::json::wvalue albumItem(
    const media_library::Album &a, const std::string &albumId,
    const std::vector<std::pair<std::string, std::string>> &albumArtists,
    int songCount, const std::string &imageTag, long long runtimeTicks);

crow::json::wvalue artistItem(const media_library::Artist &a,
                              const std::string &artistId, int albumCount,
                              const std::string &imageTag);

crow::json::wvalue genreItem(int id, const std::string &name);

// 歌单列表条目。Jellyfin 用 ChildCount / RunTimeTicks 画卡片上的曲目数与时长，
// 两个值都由调用方算好后传入（Playlist 自己带了完整曲目，算得出来）。
crow::json::wvalue playlistItem(const media_library::Playlist &p,
                                const std::string &playlistId, int childCount,
                                long long runtimeTicks);

// 歌单详情。带 ItemIds，客户端据此再拉 /Playlists/<id>/Items。
crow::json::wvalue playlistDto(const media_library::Playlist &p,
                               const std::string &playlistId);

crow::json::wvalue userDto();

crow::json::wvalue sessionInfo(const std::string &client,
                               const std::string &device);

crow::json::wvalue queryResult(std::vector<crow::json::wvalue> items, int total,
                               int startIndex);

} // namespace jellyfin

#endif
