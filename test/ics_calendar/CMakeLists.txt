add_executable(IcsCalendarTest
  IcsCalendarTest.cpp
  ${REPO_ROOT}/lib/ICal/IcsDate.cpp
  ${REPO_ROOT}/lib/ICal/IcsCalendar.cpp
  ${REPO_ROOT}/lib/ICal/IcsEditor.cpp
)

target_include_directories(IcsCalendarTest PRIVATE
  ${REPO_ROOT}/lib/ICal
)

target_link_libraries(IcsCalendarTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(IcsCalendarTest)
