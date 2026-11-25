cmake_minimum_required(VERSION 3.23)
project(musicd)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Release)

# ---------------------------------------------------------------------------
# vcpkg manifest 模式
#
# 依赖版本由 vcpkg.json 的 builtin-baseline 决定；外部 checkout 必须包含该
# commit，否则 registry 里的 versions 与实际可安装的 port 不一致，问题会推迟
# 到构建期才暴露。这里在 configure 阶段做一次廉价的预检。
# ---------------------------------------------------------------------------
if(NOT DEFINED ENV{VCPKG_ROOT})
  message(FATAL_ERROR "未设置 VCPKG_ROOT（例如 export VCPKG_ROOT=/opt/vcpkg）")
endif()
set(MUSICD_VCPKG_ROOT "$ENV{VCPKG_ROOT}")

if(NOT EXISTS "${MUSICD_VCPKG_ROOT}/vcpkg")
  message(FATAL_ERROR "VCPKG_ROOT=${MUSICD_VCPKG_ROOT} 下找不到 vcpkg 可执行文件")
endif()

set(MUSICD_VCPKG_BASELINE "320fc5d9fb666ebb1e76a08e63870e87a2063fe9")
execute_process(
  COMMAND git -C "${MUSICD_VCPKG_ROOT}" cat-file -e "${MUSICD_VCPKG_BASELINE}^{commit}"
  RESULT_VARIABLE MUSICD_BASELINE_CHECK
  OUTPUT_QUIET ERROR_QUIET
)
if(MUSICD_BASELINE_CHECK)
  message(FATAL_ERROR
    "vcpkg checkout 缺少 baseline ${MUSICD_VCPKG_BASELINE}；"
    "更新 ${MUSICD_VCPKG_ROOT}，或同步调整 vcpkg.json 的 builtin-baseline")
endif()

add_executable(musicd
  ./src/config.cpp
  ./src/json_util.cpp
  ./src/db.cpp
  ./src/db/session.cpp
  ./src/db/soci_session.cpp
  ./src/schema.cpp
  ./src/schema_pg.cpp
  ./src/storage.cpp
  ./src/image_cache.cpp
  ./src/database.cpp
  ./src/service.cpp
  ./src/metadata.cpp
  ./src/scanner.cpp
  ./src/http_util.cpp
  ./src/http_server.cpp
  ./src/musicd.cpp
  ./src/media_library/source_ref.cpp
  ./src/media_library/local_library.cpp
  ./src/media_library/library_runtime.cpp
  ./src/media_library/registry.cpp
  ./src/media_library/entity_link.cpp
  ./src/media_library/router.cpp
  ./src/media_library/media_router.cpp
  ./src/media_library/media_runtime.cpp
  ./src/media_library/library_directory.cpp
  ./src/media_library/byte_stream.cpp
  ./src/media_library/playback.cpp
  ./src/media_library/artwork.cpp
  ./src/media_library/netease_library.cpp
  ./src/media_library/lyric_parser.cpp
  ./src/tools/lyrics_ttml.cpp
  ./src/media_library/playlist_directory.cpp
  ./src/tools/amll_ttml.cpp
  ./src/stream_http.cpp
  ./src/api/subsonic/subsonic.cpp
  ./src/api/subsonic/access_log.cpp
  ./src/api/subsonic/envelope.cpp
  ./src/api/subsonic/dto.cpp
  ./src/api/subsonic/params.cpp
  ./src/api/v1/v1.cpp
  ./src/api/api-enhanced/transport.cpp
  ./src/api/api-enhanced/ids.cpp
  ./src/api/api-enhanced/client.cpp
  ./src/api/jellyfin/ids.cpp
  ./src/api/jellyfin/dto.cpp
  ./src/api/jellyfin/jellyfin.cpp
  ./src/api/playlist/playlist.cpp
  ./src/api/webdav/webdav.cpp
  ./src/api/webdav/webdav_stream.cpp
)

# 全部依赖来自 vcpkg manifest（见 vcpkg.json）。
find_package(asio CONFIG REQUIRED)
find_package(SOCI CONFIG REQUIRED COMPONENTS Core PostgreSQL SQLite3)
# Drogon 必须放在 SOCI 之后：它的 DrogonConfig.cmake 会把自带 FindSQLite3.cmake
# 塞进 CMAKE_MODULE_PATH，遮住 CMake 内置模块，导致 SQLite3::SQLite3 目标缺失。
find_package(Drogon CONFIG REQUIRED)
find_package(TagLib CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED)

set_target_properties(musicd PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

target_compile_options(musicd PRIVATE -g)

# asio 以 standalone 方式使用（不依赖 Boost）。ASIO_NO_DEPRECATED 显式保留，
# 避免弃用接口被重新启用。
target_compile_definitions(musicd PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)

# sqlite3 直接用于 src/db/soci_session.cpp（#include <sqlite3.h>），
# 除 SOCI 的后端传递外再显式链接一次，保证头文件路径可用。
target_link_libraries(musicd PRIVATE
  Drogon::Drogon
  asio::asio
  SOCI::SOCI
  unofficial::sqlite3::sqlite3
  TagLib::tag
  pugixml::pugixml
  PostgreSQL::PostgreSQL
  OpenSSL::SSL
  OpenSSL::Crypto
)

target_include_directories(musicd PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/jellyfin
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/playlist
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/webdav
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/api-enhanced
)

# 测试程序（C++）：test/ 目录。均为独立可执行文件，手工运行。
add_executable(playlist_test
  ./test/playlist_test.cpp
  ./src/json_util.cpp
)
target_link_libraries(playlist_test PRIVATE asio::asio Drogon::Drogon)
target_compile_definitions(playlist_test PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_include_directories(playlist_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(playlist_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# 歌词测试：需要 TagLib 写入内嵌歌词。
add_executable(lyrics_test
  ./test/lyrics_test.cpp
  ./src/json_util.cpp
)
target_link_libraries(lyrics_test PRIVATE asio::asio TagLib::tag Drogon::Drogon)
target_compile_definitions(lyrics_test PRIVATE ASIO_STANDALONE)
target_include_directories(lyrics_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(lyrics_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# 艺术家解析测试：需要 TagLib 写入 ARTIST 标签。
add_executable(artists_test
  ./test/artists_test.cpp
  ./src/json_util.cpp
)
target_link_libraries(artists_test PRIVATE asio::asio TagLib::tag Drogon::Drogon)
target_compile_definitions(artists_test PRIVATE ASIO_STANDALONE)
target_include_directories(artists_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(artists_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# AMLL TTML 正反解析测试：只依赖 pugixml，不需要 TagLib。
add_executable(amll_ttml_test
  ./test/amll_ttml_test.cpp
  ./src/tools/amll_ttml.cpp
)
target_include_directories(amll_ttml_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(amll_ttml_test PRIVATE pugixml::pugixml)
set_target_properties(amll_ttml_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
)