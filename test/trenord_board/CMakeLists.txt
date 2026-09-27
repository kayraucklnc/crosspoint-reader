add_executable(TrenordBoardTest
  TrenordBoardTest.cpp
  ${REPO_ROOT}/src/network/TrenordBoardParser.cpp
  ${REPO_ROOT}/src/network/JsonPathScanner.cpp
  ${REPO_ROOT}/src/network/Aes256Ecb.cpp
)

target_include_directories(TrenordBoardTest PRIVATE ${REPO_ROOT}/src/network)

target_compile_definitions(TrenordBoardTest PRIVATE
  TRENORD_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(TrenordBoardTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TrenordBoardTest)
