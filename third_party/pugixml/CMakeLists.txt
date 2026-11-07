# pugixml 1.16（MIT，见同目录 LICENSE.md）。
#
# 只被 src/tools/amll_ttml.cpp 用于解析 AMLL TTML 歌词，别处不要再引。
# 单文件实现：只编译 pugixml.cpp，头文件目录随 PUBLIC 导出。
add_library(pugixml STATIC pugixml.cpp)

target_include_directories(pugixml PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 第三方代码：不要本项目的告警等级，也不要参与 -g 调试构建。
target_compile_options(pugixml PRIVATE -w)
