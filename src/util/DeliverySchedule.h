#pragma once

#include <ctime>
#include <cstdint>
#include <string>

/**
 * Pure scheduling arithmetic for automated book delivery.
 *
 * Free of Arduino/ESP-IDF includes so it can be unit tested on the host.
 * All inputs are UTC epochs; the local calendar comes from libc's localtime_r,
 * which the firmware configures through timezones::applyToClock() and the tests
 * configure with TZ + tzset(). That indirection is deliberate: the POSIX TZ
 * rules carry DST, so "the next 06:00 local" stays correct across the changeover
 * instead of drifting by an hour twice a year.
 *
 * Delivery is keyed on the LOCAL date: at most one book per local day, fetched
 * at or after the configured local hour.
 */
namespace DeliverySchedule {

// The RTC reads as garbage before it is ever set. Anything earlier than this
// instant (2023-11-14 22:13:20 UTC) means "clock not yet synced".
constexpr time_t MIN_VALID_EPOCH = 1700000000;

// How long to wait before retrying when the clock cannot be trusted.
constexpr uint32_t RETRY_INTERVAL_SEC = 3600;

constexpr uint8_t MIN_DELIVERY_HOUR = 0;
constexpr uint8_t MAX_DELIVERY_HOUR = 23;

/** Whether the clock has been set and can be reasoned about. */
bool isClockValid(time_t nowUtc);

/**
 * Seconds to sleep until the next occurrence of deliveryHour in local time.
 * Landing exactly on the hour yields the following day, so the sleep taken
 * right after a delivery does not wake the device straight back up.
 * With an unusable clock, returns RETRY_INTERVAL_SEC.
 *
 * Crossing a DST boundary the gap is 23 or 25 hours, not 24 - the result is
 * derived from the calendar, never from adding 86400.
 */
uint32_t secondsUntilNext(time_t nowUtc, uint8_t deliveryHour);

/** SD path for the book of the local day containing nowUtc, "/YYYY-MM-DD.epub". */
std::string fileNameFor(time_t nowUtc);

/**
 * Whether a delivery is owed right now.
 * An unusable clock counts as due: the device must connect and sync before it
 * can tell, and the caller re-checks once the time is known.
 */
bool isDue(time_t nowUtc, time_t lastFetchUtc, uint8_t deliveryHour);

}  // namespace DeliverySchedule
