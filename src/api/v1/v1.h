#ifndef API_V1_V1_H
#define API_V1_V1_H

#include "http_server.h"

#include <functional>

#include <string>

// 原生统一读门面：直接依赖 MediaRouter + LibraryDirectory，
// 不做选源、不做组合、不做平台翻译；JSON 序列化只发生在这一层。
namespace api_v1 {

http_server::ResponsePtr libraries(const http_server::RequestPtr &req);
http_server::ResponsePtr provider_health(const http_server::RequestPtr &req);

http_server::ResponsePtr search(const http_server::RequestPtr &req);

http_server::ResponsePtr track(const http_server::RequestPtr &req, const std::string &ref);
void track_stream(const http_server::RequestPtr &req, const std::string &ref,
                  std::function<void(const http_server::ResponsePtr &)> callback);
http_server::ResponsePtr track_lyrics(const http_server::RequestPtr &req, const std::string &ref);
void track_artwork(const http_server::RequestPtr &req, const std::string &ref,
                   std::function<void(const http_server::ResponsePtr &)> callback);

http_server::ResponsePtr album(const http_server::RequestPtr &req, const std::string &ref);
http_server::ResponsePtr album_tracks(const http_server::RequestPtr &req, const std::string &ref);
http_server::ResponsePtr artist(const http_server::RequestPtr &req, const std::string &ref);
http_server::ResponsePtr artist_albums(const http_server::RequestPtr &req, const std::string &ref);

http_server::ResponsePtr playlists(const http_server::RequestPtr &req);
http_server::ResponsePtr playlist(const http_server::RequestPtr &req, const std::string &ref);

// 实体链接：以 SourceRef 为句柄，内部 link_id 不出进程。
http_server::ResponsePtr entity_link(const http_server::RequestPtr &req, const std::string &ref);
http_server::ResponsePtr entity_link_order(const http_server::RequestPtr &req,
                                 const std::string &ref);
http_server::ResponsePtr entity_link_create(const http_server::RequestPtr &req);
http_server::ResponsePtr entity_link_delete(const http_server::RequestPtr &req,
                                  const std::string &ref);
http_server::ResponsePtr entity_link_remove_member(const http_server::RequestPtr &req,
                                         const std::string &anchor,
                                         const std::string &member);
http_server::ResponsePtr entity_link_set_overrides(const http_server::RequestPtr &req,
                                         const std::string &ref);
http_server::ResponsePtr entity_link_clear_override(const http_server::RequestPtr &req,
                                          const std::string &ref,
                                          const std::string &aspect);

// 播放会话（7.1–7.4）：字节只从会话出，locator 不出进程。
http_server::ResponsePtr playback_open(const http_server::RequestPtr &req, const std::string &ref);
http_server::ResponsePtr playback_session(const http_server::RequestPtr &req,
                                const std::string &session_id);
http_server::ResponsePtr playback_refresh(const http_server::RequestPtr &req,
                                const std::string &session_id);
http_server::ResponsePtr playback_close(const http_server::RequestPtr &req,
                              const std::string &session_id);
void playback_stream(const http_server::RequestPtr &req,
                     const std::string &session_id,
                     std::function<void(const http_server::ResponsePtr &)> callback);

} // namespace api_v1

#endif // API_V1_V1_H
