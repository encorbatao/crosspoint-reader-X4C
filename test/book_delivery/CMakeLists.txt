add_executable(DeliveryScheduleTest
  DeliveryScheduleTest.cpp
  ${REPO_ROOT}/src/util/DeliverySchedule.cpp
)

target_include_directories(DeliveryScheduleTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(DeliveryScheduleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DeliveryScheduleTest)
