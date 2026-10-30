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

crow::json::wvalue userDto();

crow::json::wvalue sessionInfo(const std::string &client,
                               const std::string &device);

crow::json::wvalue queryResult(std::vector<crow::json::wvalue> items, int total,
                               int startIndex);

} // namespace jellyfin

#endif
