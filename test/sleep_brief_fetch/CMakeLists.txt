add_executable(SleepBriefFetchTest
  SleepBriefFetchTest.cpp
  ${REPO_ROOT}/src/activities/boot_sleep/SleepBriefFetch.cpp
  ${REPO_ROOT}/src/activities/boot_sleep/SleepBrief.cpp
  ${REPO_ROOT}/lib/LunarCalendar/LunarCalendar.cpp
  ${REPO_ROOT}/lib/HttpTransportPolicy/HttpTransportPolicy.cpp
)

target_include_directories(SleepBriefFetchTest PRIVATE
  ${REPO_ROOT}/src/activities/boot_sleep
  ${REPO_ROOT}/lib/LunarCalendar
  ${REPO_ROOT}/lib/HttpTransportPolicy
)

target_link_libraries(SleepBriefFetchTest PRIVATE
  ArduinoJson
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SleepBriefFetchTest)
