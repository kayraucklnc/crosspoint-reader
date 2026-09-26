add_executable(MinesweeperBoardTest
  MinesweeperBoardTest.cpp
  ../../src/activities/play/MinesweeperBoard.cpp
)

target_include_directories(MinesweeperBoardTest PRIVATE ../../src)

target_link_libraries(MinesweeperBoardTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MinesweeperBoardTest)
