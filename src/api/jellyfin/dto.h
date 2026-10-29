#ifndef JELLYFIN_DTO_H
#define JELLYFIN_DTO_H

#include "crow/json.h"
#include "database.h"
#include "media_library/domain.h"

#include <string>
#include <utility>
#include <vector>

namespace jellyfin {

crow::json::wvalue userData(const repo::Track &t);

crow::json::wvalue trackItem(
    const repo::Track &t, const std::string &albumName,
    const std::vector<std::pair<std::string, int>> &trackArtists,
    const std::vector<std::pair<std::string, int>> &albumArtists, int year,
    const std::string &imageTag,
    const std::vector<std::pair<std::string, int>> &genres,
    bool hasLyrics = false);

// 领域模型版本：路由只持有统一领域模型，compat 层负责映射成 Jellyfin DTO。
crow::json::wvalue trackItem(
    const media_library::Track &t, int trackId, int albumId,
    const std::string &albumName, const std::string &filePath,
    const std::vector<std::pair<std::string, int>> &trackArtists,
    const std::vector<std::pair<std::string, int>> &albumArtists,
    const std::vector<std::pair<std::string, int>> &genres);

crow::json::wvalue albumItem(
    const repo::Album &a,
    const std::vector<std::pair<std::string, int>> &albumArtists, int songCount,
    const std::string &imageTag, long long runtimeTicks);

crow::json::wvalue albumItem(
    const media_library::Album &a, int albumId,
    const std::vector<std::pair<std::string, int>> &albumArtists, int songCount,
    const std::string &imageTag, long long runtimeTicks);

crow::json::wvalue artistItem(const repo::Artist &a, int albumCount,
                              const std::string &imageTag);

crow::json::wvalue artistItem(const media_library::Artist &a, int artistId,
                              int albumCount, const std::string &imageTag);

crow::json::wvalue genreItem(int id, const std::string &name);

crow::json::wvalue userDto();

crow::json::wvalue sessionInfo(const std::string &client,
                               const std::string &device);

crow::json::wvalue queryResult(std::vector<crow::json::wvalue> items, int total,
                               int startIndex);

} // namespace jellyfin

#endif
