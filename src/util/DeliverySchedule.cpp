#include "DeliverySchedule.h"

#include <cstdio>

namespace DeliverySchedule {

namespace {

// Local wall-clock fields for a UTC epoch. localtime_r consults the TZ rule the
// firmware installed via timezones::applyToClock(), so DST is already folded in.
struct tm localFieldsOf(const time_t utc) {
  struct tm fields = {};
  localtime_r(&utc, &fields);
  return fields;
}

// The instant of deliveryHour:00:00 local on the calendar day `fields` names.
// tm_isdst = -1 hands the DST decision to mktime, which is what keeps the
// result honest on a changeover day.
time_t slotOn(struct tm fields, const uint8_t deliveryHour) {
  fields.tm_hour = deliveryHour;
  fields.tm_min = 0;
  fields.tm_sec = 0;
  fields.tm_isdst = -1;

  return mktime(&fields);
}

// Same local date?  tm_yday alone repeats every year, so the year comes too.
bool isSameLocalDay(const struct tm& a, const struct tm& b) {
  return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
}

}  // namespace

bool isClockValid(const time_t nowUtc) { return nowUtc >= MIN_VALID_EPOCH; }

uint32_t secondsUntilNext(const time_t nowUtc, const uint8_t deliveryHour) {
  // Without a set clock there is no way to locate the next slot. Come back in
  // an hour, by which point a fetch attempt may have synced the RTC.
  if (!isClockValid(nowUtc)) {
    return RETRY_INTERVAL_SEC;
  }

  const struct tm now = localFieldsOf(nowUtc);

  time_t target = slotOn(now, deliveryHour);

  // Past today's slot, or exactly on it, so aim at tomorrow's. The day is
  // advanced on the calendar and re-resolved rather than by adding 86400:
  // across a DST boundary the next 06:00 is 23 or 25 hours away, not 24.
  if (target <= nowUtc) {
    struct tm tomorrow = now;
    tomorrow.tm_mday += 1;  // mktime normalises month/year overflow
    target = slotOn(tomorrow, deliveryHour);
  }

  return static_cast<uint32_t>(target - nowUtc);
}

std::string fileNameFor(const time_t nowUtc) {
  const struct tm fields = localFieldsOf(nowUtc);

  char buffer[sizeof("/YYYY-MM-DD.epub")];
  snprintf(buffer, sizeof(buffer), "/%04d-%02d-%02d.epub", fields.tm_year + 1900, fields.tm_mon + 1, fields.tm_mday);

  return buffer;
}

bool isDue(const time_t nowUtc, const time_t lastFetchUtc, const uint8_t deliveryHour) {
  // The RTC was lost. Whether a book is owed can only be settled after a sync,
  // so report due and let the caller ask again once the time is known.
  if (!isClockValid(nowUtc)) {
    return true;
  }

  const struct tm now = localFieldsOf(nowUtc);
  if (now.tm_hour < deliveryHour) {
    return false;
  }

  if (!isClockValid(lastFetchUtc)) {
    return true;
  }

  // One book per local day. A slot missed by several days is not backfilled -
  // the URL serves today's content either way.
  return !isSameLocalDay(now, localFieldsOf(lastFetchUtc));
}

}  // namespace DeliverySchedule
