add_executable(SleepBriefTest
  SleepBriefTest.cpp
  ${REPO_ROOT}/src/activities/boot_sleep/SleepBrief.cpp
  ${REPO_ROOT}/lib/LunarCalendar/LunarCalendar.cpp
)

target_include_directories(SleepBriefTest PRIVATE
  ${REPO_ROOT}/src/activities/boot_sleep
  ${REPO_ROOT}/lib/LunarCalendar
)

target_link_libraries(SleepBriefTest PRIVATE
  ArduinoJson
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SleepBriefTest)
