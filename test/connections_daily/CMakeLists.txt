add_executable(ConnectionsDailyTest
  ConnectionsDailyTest.cpp
  ../../src/activities/play/ConnectionsDaily.cpp
  ../../src/activities/play/ConnectionsResults.cpp
)

target_include_directories(ConnectionsDailyTest PRIVATE ../../src)

target_link_libraries(ConnectionsDailyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ConnectionsDailyTest)
