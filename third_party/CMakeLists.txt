add_subdirectory(./asio-asio-1-36-0/)

set(ASIO_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/asio-asio-1-36-0/asio/include/")

add_subdirectory(./Crow-1.3.1/)
add_subdirectory(./SQLiteCpp-3.3.3/)

include(FetchContent)

# SOCI 4.1.4：configure 阶段从 GitHub 拉取源码
# 放在 SQLiteCpp 之后，复用它的 SQLite::SQLite3 目标，避免再编译一份
# sqlite3.c。只拉取 fmt 子模块（SOCI_FMT_BUILTIN 需要）；sqlite3 子模块
# 不拉取。其余后端显式关闭，防止 AUTO 去探测系统里的 MySQL/PostgreSQL
# 客户端库。
set(SOCI_STATIC ON CACHE BOOL "" FORCE)
set(SOCI_SHARED OFF CACHE BOOL "" FORCE)
set(SOCI_TESTS OFF CACHE BOOL "" FORCE)
set(SOCI_INSTALL OFF CACHE BOOL "" FORCE)
set(SOCI_LTO OFF CACHE BOOL "" FORCE)
set(WITH_BOOST OFF CACHE BOOL "" FORCE)
set(SOCI_FMT_BUILTIN ON CACHE STRING "" FORCE)
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
  GIT_REPOSITORY https://github.com/SOCI/soci.git
  # v4.1.4 的 tag 对象解引用后的 commit，固定版本便于复现。
  GIT_TAG 3b09d4e8b544a35774d4779eeb805a8e589f3c05
  GIT_SUBMODULES "3rdparty/fmt"
  GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(SOCI)

add_subdirectory(./taglib-2.3.2/)
add_subdirectory(./pugixml/)

# - cpr：仅被 src/crptext.cpp 使用，该文件已随 P0 移除；顺带不再拉取 curl/zlib
