add_subdirectory(./asio-asio-1-36-0/)

set(ASIO_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/asio-asio-1-36-0/asio/include/")

add_subdirectory(./Crow-1.3.1/)
add_subdirectory(./SQLiteCpp-3.3.3/)
add_subdirectory(./taglib-2.3.2/)

# - cpr：仅被 src/crptext.cpp 使用，该文件已随 P0 移除；顺带不再拉取 curl/zlib
