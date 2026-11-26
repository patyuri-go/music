# 单元测试（GTest）。
#
# 这里的测试只依赖纯函数，不读 config.json、不连数据库、不访问网络；
# 后续新增模块测试时，把源文件加到 unit_tests 即可，CTest 会自动发现用例。

# jsoncpp 是网易云客户端的直接依赖；生产构建通过 Drogon 传递拿到它，
# 测试是新的直接消费者，所以在这里显式声明。
find_package(jsoncpp CONFIG REQUIRED)

add_executable(unit_tests
  ./source_ref_test.cpp
  ./amll_ttml_test.cpp
  ./netease_client_numeric_test.cpp
  ./json_util_test.cpp
  ./router_test.cpp
  ${PROJECT_SOURCE_DIR}/src/api/api-enhanced/client.cpp
  ${PROJECT_SOURCE_DIR}/src/json_util.cpp
  ${PROJECT_SOURCE_DIR}/src/media_library/source_ref.cpp
  ${PROJECT_SOURCE_DIR}/src/media_library/aspect.cpp
  ${PROJECT_SOURCE_DIR}/src/media_library/registry.cpp
  ${PROJECT_SOURCE_DIR}/src/media_library/router.cpp
  ${PROJECT_SOURCE_DIR}/src/tools/amll_ttml.cpp
)

target_include_directories(unit_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(unit_tests PRIVATE
  GTest::gtest_main
  JsonCpp::JsonCpp
  pugixml::pugixml
)

set_target_properties(unit_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${MUSICD_TEST_BIN_DIR}"
)

include(GoogleTest)
gtest_discover_tests(unit_tests)