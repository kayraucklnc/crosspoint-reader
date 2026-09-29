add_executable(IncomeFetchTest
  IncomeFetchTest.cpp
  ${REPO_ROOT}/src/network/ChargeDayTally.cpp
  ${REPO_ROOT}/src/network/StripeChargeScanner.cpp
  ${REPO_ROOT}/src/network/NetLog.cpp
)

target_include_directories(IncomeFetchTest PRIVATE ${REPO_ROOT}/src/network)

target_compile_definitions(IncomeFetchTest PRIVATE NET_LOG_HOST_TEST)

target_link_libraries(IncomeFetchTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(IncomeFetchTest)
