add_subdirectory(./asio-asio-1-36-0/)

set(ASIO_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/asio-asio-1-36-0/asio/include/")

add_subdirectory(./Crow-1.3.1/)
add_subdirectory(./SQLiteCpp-3.3.3/)

include(FetchContent)

# SOCI 4.1.4：configure 阶段直接下载 release tarball，不把源码放进
# third_party。tarball 不含子模块，所以 fmt 单独用 FetchContent 拉取；
# sqlite3 不拉取，复用 SQLiteCpp 的 SQLite::SQLite3 目标，避免再编译一份
# sqlite3.c。其余后端显式关闭，防止 AUTO 去探测系统里的 MySQL/PostgreSQL
# 客户端库。
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  fmt
  URL https://github.com/fmtlib/fmt/archive/407c905e45ad75fc29bf0f9bb7c5c2fd3475976f.tar.gz
  URL_HASH SHA256=2bc1fe4a5b6d5d6a614239b4ca1d520e66e152a02d3d262684d26dfd6ab3438a
)
FetchContent_MakeAvailable(fmt)

set(SOCI_STATIC ON CACHE BOOL "" FORCE)
set(SOCI_SHARED OFF CACHE BOOL "" FORCE)
set(SOCI_TESTS OFF CACHE BOOL "" FORCE)
set(SOCI_INSTALL OFF CACHE BOOL "" FORCE)
set(SOCI_LTO OFF CACHE BOOL "" FORCE)
set(WITH_BOOST OFF CACHE BOOL "" FORCE)
# 不使用 SOCI 自带的 fmt 子目录；上面的 fmt 目标已存在，SOCI 会直接复用它。
set(SOCI_FMT_BUILTIN OFF CACHE STRING "" FORCE)
set(SOCI_SQLITE3 ON CACHE STRING "" FORCE)
set(SOCI_DB2 OFF CACHE STRING "" FORCE)
set(SOCI_EMPTY OFF CACHE STRING "" FORCE)
set(SOCI_FIREBIRD OFF CACHE STRING "" FORCE)
set(SOCI_MYSQL OFF CACHE STRING "" FORCE)
set(SOCI_ODBC OFF CACHE STRING "" FORCE)
set(SOCI_ORACLE OFF CACHE STRING "" FORCE)
set(SOCI_POSTGRESQL OFF CACHE STRING "" FORCE)

FetchContent_Declare(
  SOCI
  URL https://github.com/SOCI/soci/archive/refs/tags/v4.1.4.tar.gz
  URL_HASH SHA256=144f017cccc2e2d806badb3313d6ab3c67a1925bccaa747a46fb3907108a615d
)
FetchContent_MakeAvailable(SOCI)

add_subdirectory(./taglib-2.3.2/)
add_subdirectory(./pugixml/)

# - cpr：仅被 src/crptext.cpp 使用，该文件已随 P0 移除；顺带不再拉取 curl/zlib
