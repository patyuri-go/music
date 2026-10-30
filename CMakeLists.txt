cmake_minimum_required(VERSION 3.15)
project(musicd)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Release)

add_subdirectory(./third_party/)

add_executable(musicd
  ./src/config.cpp
  ./src/db.cpp
  ./src/schema.cpp
  ./src/storage.cpp
  ./src/image_cache.cpp
  ./src/database.cpp
  ./src/service.cpp
  ./src/metadata.cpp
  ./src/scanner.cpp
  ./src/http_util.cpp
  ./src/musicd.cpp
  ./src/media_library/source_ref.cpp
  ./src/media_library/entity_store.cpp
  ./src/media_library/local_library.cpp
  ./src/media_library/library_runtime.cpp
  ./src/media_library/registry.cpp
  ./src/media_library/entity_link.cpp
  ./src/media_library/router.cpp
  ./src/media_library/media_router.cpp
  ./src/api/jellyfin/ids.cpp
  ./src/api/jellyfin/dto.cpp
  ./src/api/jellyfin/jellyfin.cpp
  ./src/api/playlist/playlist.cpp
  ./src/api/webdav/webdav.cpp
  ./src/api/webdav/webdav_stream.cpp
)

find_package(OpenSSL REQUIRED)

set_target_properties(musicd PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

target_compile_options(musicd PRIVATE -g)

target_link_libraries(musicd PRIVATE Crow SQLiteCpp tag OpenSSL::SSL OpenSSL::Crypto)

# TagLib 的 CMake 目标只导出 INSTALL_INTERFACE 路径，不传播 build-tree include。
# 需手动补充三层：
#   1) taglib-2.3.2          → 让 #include <taglib/fileref.h> 解析到 taglib/ 子目录
#   2) taglib/toolkit        → 让 toolkit/ 内 #include "tfile.h" 等相对包含生效
#   3) taglib/mpeg           → 让 mpeg 子目录头文件可被找到
target_include_directories(musicd PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/jellyfin
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/playlist
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/webdav
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio-asio-1-36-0/asio/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/taglib-2.3.2
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/taglib-2.3.2/taglib
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/taglib-2.3.2/taglib/toolkit
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/taglib-2.3.2/taglib/mpeg
)

# 测试程序（C++）：test/ 目录。
add_executable(playlist_test
  ./test/playlist_test.cpp
)
target_include_directories(playlist_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Crow-1.3.1/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio-asio-1-36-0/asio/include
)
target_compile_definitions(playlist_test PRIVATE ASIO_STANDALONE)
set_target_properties(playlist_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# 歌词测试：需要 TagLib 写入内嵌歌词。
add_executable(lyrics_test
  ./test/lyrics_test.cpp
)
target_include_directories(lyrics_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Crow-1.3.1/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio-asio-1-36-0/asio/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/taglib-2.3.2
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/taglib-2.3.2/taglib
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/taglib-2.3.2/taglib/toolkit
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/taglib-2.3.2/taglib/mpeg
)
target_compile_definitions(lyrics_test PRIVATE ASIO_STANDALONE)
target_link_libraries(lyrics_test PRIVATE tag)
set_target_properties(lyrics_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# 艺术家解析测试：需要 TagLib 写入 ARTIST 标签。
add_executable(artists_test
  ./test/artists_test.cpp
)
target_include_directories(artists_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Crow-1.3.1/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio-asio-1-36-0/asio/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/taglib-2.3.2
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/taglib-2.3.2/taglib
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/taglib-2.3.2/taglib/toolkit
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/taglib-2.3.2/taglib/mpeg
)
target_compile_definitions(artists_test PRIVATE ASIO_STANDALONE)
target_link_libraries(artists_test PRIVATE tag)
set_target_properties(artists_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
