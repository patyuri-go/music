# 单元测试（GTest）。
#
# 这里的测试只依赖纯函数，不读 config.json、不连数据库、不访问网络；
# 后续新增模块测试时，把源文件加到 unit_tests 即可，CTest 会自动发现用例。

add_executable(unit_tests
  ./source_ref_test.cpp
  ${PROJECT_SOURCE_DIR}/src/media_library/source_ref.cpp
)

target_include_directories(unit_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(unit_tests PRIVATE GTest::gtest_main)

set_target_properties(unit_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

include(GoogleTest)
gtest_discover_tests(unit_tests)