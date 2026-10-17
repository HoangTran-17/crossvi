add_executable(LunarCalendarTest
  LunarCalendarTest.cpp
  ${REPO_ROOT}/lib/LunarCalendar/LunarCalendar.cpp
)

target_include_directories(LunarCalendarTest PRIVATE
  ${REPO_ROOT}/lib/LunarCalendar
)

target_link_libraries(LunarCalendarTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LunarCalendarTest)
