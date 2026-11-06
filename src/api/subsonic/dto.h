#ifndef SUBSONIC_DTO_H
#define SUBSONIC_DTO_H

#include "crow/json.h"
#include "media_library/domain.h"

#include <string>

namespace subsonic {

// 领域模型 -> Subsonic 结构。
//
// Subsonic 的 id 一律是「客户端原样回传」的不透明字符串，这里直接用
// `to_string(SourceRef)`，与 Jellyfin 门面、主 API 完全同一套 id。
// 子对象的 parent 用父级 ref 的规范串；没有父级时留空（不写该字段）。

// <child>：曲目（getSong / album 的 song[] / searchResult3 的 song[]）。
crow::json::wvalue song_dto(const media_library::Track &track);

// <albumID3>：专辑（getAlbum 的 album / searchResult3 的 albumID3）。
// include_songs 为 true 时带上 song[]（getAlbum 需要，搜索结果不需要）。
crow::json::wvalue album_dto(const media_library::Album &album,
                             bool include_songs);

// <artistID3>：艺术家（getArtist 的 artist / searchResult3 的 artistID3）。
crow::json::wvalue artist_dto(const media_library::Artist &artist,
                              int album_count);

// <artistInfo2>：艺术家描述（getArtistInfo2）。只有 biography 一个字段有值，
// 其余（musicBrainzId / lastFmUrl / *_ImageUrl）本服务不提供，不写。
// similarArtist 固定给空数组，客户端按数组取值时不必判空。
crow::json::wvalue artist_info_dto(const media_library::ArtistInfo &info);

// <albumInfo2>：专辑描述（getAlbumInfo2）。描述走 notes 字段
// （Subsonic 里专辑的说明就是这个键），其余字段同上留空不写。
crow::json::wvalue album_info_dto(const media_library::AlbumInfo &info);

// <playlist>：歌单。
crow::json::wvalue playlist_dto(const media_library::Playlist &playlist,
                                bool include_entries);

// <musicFolder>：库。
crow::json::wvalue music_folder_dto(const std::string &library_id,
                                    const std::string &name);

// <user>：Subsonic 账号。本服务不校验账号，只返回一个合成用户；
// username 为空时用 "admin" 兜底。
crow::json::wvalue user_dto(const std::string &username);

// SearchHit 命中 -> 对应 DTO（内部按 kind 分派）。
crow::json::wvalue search_hit_dto(const media_library::SearchHit &hit);

// 曲目时长换算成秒（Subsonic 用整秒）。
int duration_seconds(const media_library::Track &track);

} // namespace subsonic

#endif // SUBSONIC_DTO_H
