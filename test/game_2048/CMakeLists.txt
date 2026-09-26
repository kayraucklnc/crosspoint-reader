add_executable(Game2048BoardTest
  Game2048BoardTest.cpp
  ${REPO_ROOT}/src/activities/play/Game2048Board.cpp
)

target_include_directories(Game2048BoardTest PRIVATE ../../src)

target_link_libraries(Game2048BoardTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Game2048BoardTest)
