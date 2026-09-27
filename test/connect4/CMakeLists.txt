add_executable(Connect4Test
  Connect4Test.cpp
  ../../src/activities/play/Connect4Board.cpp
  ../../src/activities/play/Connect4Brain.cpp
)

target_include_directories(Connect4Test PRIVATE ../../src)

target_link_libraries(Connect4Test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Connect4Test)
