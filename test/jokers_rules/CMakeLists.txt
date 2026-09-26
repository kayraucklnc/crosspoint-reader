add_executable(JokersRulesTest
  JokersRulesTest.cpp
  ../../src/activities/play/JokersRules.cpp
)

target_include_directories(JokersRulesTest PRIVATE ../../src)

target_link_libraries(JokersRulesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(JokersRulesTest)
