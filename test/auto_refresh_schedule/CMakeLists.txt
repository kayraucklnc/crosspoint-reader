add_executable(AutoRefreshScheduleTest
  AutoRefreshScheduleTest.cpp
  ${REPO_ROOT}/src/autorefresh/AutoRefreshSchedule.cpp
)

target_include_directories(AutoRefreshScheduleTest PRIVATE ../../src)

target_link_libraries(AutoRefreshScheduleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AutoRefreshScheduleTest)
