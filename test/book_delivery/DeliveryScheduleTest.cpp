#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>

#include "DeliverySchedule.h"

namespace {

using namespace DeliverySchedule;

// Europe/Madrid. Chosen over a fixed-offset zone precisely because it has DST:
// the interesting scheduling bugs only appear either side of a changeover.
constexpr char CET_WITH_DST[] = "CET-1CEST,M3.5.0,M10.5.0/3";

// Reference instants, as UTC epochs, labelled by their LOCAL wall clock.
constexpr time_t SEP_20_LOCAL_MIDNIGHT = 1789855200;  // 2026-09-20 00:00 local
constexpr time_t SEP_20_LOCAL_01H = 1789858800;       // 2026-09-20 01:00 local (still Sep 19 in UTC)
constexpr time_t SEP_20_LOCAL_06H = 1789876800;       // 2026-09-20 06:00 local
constexpr time_t SEP_20_LOCAL_10H = 1789891200;       // 2026-09-20 10:00 local
constexpr time_t SEP_19_LOCAL_06H = 1789790400;       // 2026-09-19 06:00 local
constexpr time_t MAR_28_LOCAL_06H = 1774674000;       // day before spring forward
constexpr time_t OCT_24_LOCAL_06H = 1792814400;       // day before fall back
constexpr time_t FEB_29_LOCAL_12H = 1709204400;       // 2024-02-29, leap day

constexpr uint8_t SIX_AM = 6;

constexpr uint32_t ONE_HOUR = 3600;
constexpr uint32_t ONE_DAY = 86400;

class DeliveryScheduleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    setenv("TZ", CET_WITH_DST, 1);
    tzset();
  }

  void TearDown() override {
    unsetenv("TZ");
    tzset();
  }
};

TEST_F(DeliveryScheduleTest, ClockValidityBoundary) {
  EXPECT_FALSE(isClockValid(0));
  EXPECT_FALSE(isClockValid(MIN_VALID_EPOCH - 1));
  EXPECT_TRUE(isClockValid(MIN_VALID_EPOCH));
  EXPECT_TRUE(isClockValid(SEP_20_LOCAL_10H));
}

TEST_F(DeliveryScheduleTest, SleepsUntilLaterToday) {
  EXPECT_EQ(secondsUntilNext(SEP_20_LOCAL_MIDNIGHT, SIX_AM), 6 * ONE_HOUR);
}

TEST_F(DeliveryScheduleTest, SleepsUntilTomorrowWhenHourPassed) {
  EXPECT_EQ(secondsUntilNext(SEP_20_LOCAL_10H, SIX_AM), 20 * ONE_HOUR);
}

// Landing exactly on the hour must yield a full day: this is the sleep taken
// immediately after a delivery, and zero would wake the device straight back up.
TEST_F(DeliveryScheduleTest, SleepsFullDayWhenExactlyOnHour) {
  EXPECT_EQ(secondsUntilNext(SEP_20_LOCAL_06H, SIX_AM), ONE_DAY);
}

// Clocks go forward: 06:00 tomorrow is only 23 hours away. Adding 86400 here
// would wake the device an hour late for the rest of the summer.
TEST_F(DeliveryScheduleTest, SpringForwardDayIsTwentyThreeHours) {
  EXPECT_EQ(secondsUntilNext(MAR_28_LOCAL_06H, SIX_AM), 23 * ONE_HOUR);
}

// Clocks go back: 25 hours.
TEST_F(DeliveryScheduleTest, FallBackDayIsTwentyFiveHours) {
  EXPECT_EQ(secondsUntilNext(OCT_24_LOCAL_06H, SIX_AM), 25 * ONE_HOUR);
}

TEST_F(DeliveryScheduleTest, UnusableClockRetriesInAnHour) {
  EXPECT_EQ(secondsUntilNext(0, SIX_AM), RETRY_INTERVAL_SEC);
}

TEST_F(DeliveryScheduleTest, FileNameUsesLocalDate) {
  EXPECT_EQ(fileNameFor(SEP_20_LOCAL_10H), "/2026-09-20.epub");
}

// 01:00 local is still the previous day in UTC. The book belongs to the local
// day, so the name must not follow UTC.
TEST_F(DeliveryScheduleTest, FileNameFollowsLocalDateNotUtc) {
  EXPECT_EQ(fileNameFor(SEP_20_LOCAL_01H), "/2026-09-20.epub");
}

TEST_F(DeliveryScheduleTest, FileNameHandlesLeapDay) {
  EXPECT_EQ(fileNameFor(FEB_29_LOCAL_12H), "/2024-02-29.epub");
}

TEST_F(DeliveryScheduleTest, DueWhenNeverFetched) { EXPECT_TRUE(isDue(SEP_20_LOCAL_10H, 0, SIX_AM)); }

TEST_F(DeliveryScheduleTest, DueWhenClockUnusable) { EXPECT_TRUE(isDue(0, SEP_19_LOCAL_06H, SIX_AM)); }

TEST_F(DeliveryScheduleTest, DueOnNewDayAfterHour) {
  EXPECT_TRUE(isDue(SEP_20_LOCAL_10H, SEP_19_LOCAL_06H, SIX_AM));
}

TEST_F(DeliveryScheduleTest, NotDueBeforeHourOnNewDay) {
  EXPECT_FALSE(isDue(SEP_20_LOCAL_MIDNIGHT, SEP_19_LOCAL_06H, SIX_AM));
}

TEST_F(DeliveryScheduleTest, NotDueWhenAlreadyFetchedToday) {
  EXPECT_FALSE(isDue(SEP_20_LOCAL_10H, SEP_20_LOCAL_06H, SIX_AM));
}

// A slot missed by several days is not backfilled; the next one still fires.
TEST_F(DeliveryScheduleTest, DueAfterLongGap) {
  EXPECT_TRUE(isDue(SEP_20_LOCAL_10H, SEP_20_LOCAL_10H - 10 * ONE_DAY, SIX_AM));
}

// Year rollover: December 31 to January 1 must read as a new day.
TEST_F(DeliveryScheduleTest, DueAcrossYearBoundary) {
  struct tm newYear = {};
  newYear.tm_year = 127;  // 2027
  newYear.tm_mon = 0;
  newYear.tm_mday = 1;
  newYear.tm_hour = 10;
  newYear.tm_isdst = -1;
  const time_t jan1 = mktime(&newYear);

  EXPECT_TRUE(isDue(jan1, jan1 - ONE_DAY, SIX_AM));
}

}  // namespace
