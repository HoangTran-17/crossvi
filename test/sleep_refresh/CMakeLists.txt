add_executable(SleepRefreshScheduleTest
  SleepRefreshScheduleTest.cpp
)

target_include_directories(SleepRefreshScheduleTest PRIVATE
  ${REPO_ROOT}/src/activities/boot_sleep
)

target_link_libraries(SleepRefreshScheduleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SleepRefreshScheduleTest)
