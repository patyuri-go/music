#ifndef API_V1_V1_H
#define API_V1_V1_H

#include "crow/http_request.h"
#include "crow/http_response.h"

#include <string>

// 原生统一读门面：直接依赖 MediaRouter + LibraryDirectory，
// 不做选源、不做组合、不做平台翻译；JSON 序列化只发生在这一层。
namespace api_v1 {

crow::response libraries(const crow::request &req);
crow::response provider_health(const crow::request &req);

crow::response search(const crow::request &req);

crow::response track(const crow::request &req, const std::string &ref);
crow::response track_stream(const crow::request &req, const std::string &ref);
crow::response track_lyrics(const crow::request &req, const std::string &ref);
crow::response track_artwork(const crow::request &req, const std::string &ref);

crow::response album(const crow::request &req, const std::string &ref);
crow::response album_tracks(const crow::request &req, const std::string &ref);
crow::response artist(const crow::request &req, const std::string &ref);
crow::response artist_albums(const crow::request &req, const std::string &ref);

crow::response playlists(const crow::request &req);
crow::response playlist(const crow::request &req, const std::string &ref);

// 播放会话（7.1–7.4）：字节只从会话出，locator 不出进程。
crow::response playback_open(const crow::request &req, const std::string &ref);
crow::response playback_session(const crow::request &req,
                                const std::string &session_id);
crow::response playback_refresh(const crow::request &req,
                                const std::string &session_id);
crow::response playback_close(const crow::request &req,
                              const std::string &session_id);
crow::response playback_stream(const crow::request &req,
                               const std::string &session_id);

} // namespace api_v1

#endif // API_V1_V1_H
