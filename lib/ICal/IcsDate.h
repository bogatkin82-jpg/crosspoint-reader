#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>

// Calendar arithmetic on a proleptic Gregorian day number (days since
// 1970-01-01). Pure integer code: no libc time functions, host-testable.
namespace ics {

constexpr int32_t NO_DAY = INT32_MIN;

int32_t daysFromCivil(int y, unsigned m, unsigned d);
void civilFromDays(int32_t z, int& y, unsigned& m, unsigned& d);
// 0 = Monday ... 6 = Sunday
int weekdayMon0(int32_t day);
int daysInMonth(int y, unsigned m);
// Day number of the Monday that starts the week containing `day`.
inline int32_t mondayOf(int32_t day) { return day - weekdayMon0(day); }

// A DATE or DATE-TIME value. minute < 0 means a DATE (all-day) value.
struct DateTime {
  int32_t day = NO_DAY;
  int16_t minute = -1;
  bool utc = false;
  bool valid() const { return day != NO_DAY; }
  bool allDay() const { return minute < 0; }
};

// Parses "YYYYMMDD" or "YYYYMMDDTHHMM[SS][Z]". Seconds are dropped.
bool parseDateTime(const char* s, size_t len, DateTime& out);

// Converts a UTC epoch (seconds) to local wall-clock day/minute. The device
// binds this to localtime_r under the active POSIX TZ; tests use identity.
using UtcToLocalFn = void (*)(int64_t epochSec, int32_t& day, int16_t& minute);
void utcIdentity(int64_t epochSec, int32_t& day, int16_t& minute);

// Writes "YYYYMMDDTHHMMSSZ" (17 bytes incl. NUL) for a UTC epoch.
void formatUtcStamp(int64_t epochSec, char* out, size_t outSize);
// Writes "YYYYMMDD" (9 bytes incl. NUL).
void formatDate(int32_t day, char* out, size_t outSize);

}  // namespace ics
