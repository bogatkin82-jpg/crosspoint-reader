#include "IcsDate.h"

#include <cstdio>

namespace ics {

// Howard Hinnant's days_from_civil / civil_from_days.
int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

void civilFromDays(int32_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
}

int weekdayMon0(int32_t day) {
  // 1970-01-01 was a Thursday (Mon0 index 3).
  int w = static_cast<int>((static_cast<int64_t>(day) + 3) % 7);
  return w < 0 ? w + 7 : w;
}

int daysInMonth(int y, unsigned m) {
  static constexpr uint8_t DIM[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2) {
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return leap ? 29 : 28;
  }
  return (m >= 1 && m <= 12) ? DIM[m - 1] : 30;
}

namespace {
bool digits(const char* s, int n, int& out) {
  out = 0;
  for (int i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    out = out * 10 + (s[i] - '0');
  }
  return true;
}
}  // namespace

bool parseDateTime(const char* s, size_t len, DateTime& out) {
  out = DateTime{};
  if (len < 8) return false;
  int y, m, d;
  if (!digits(s, 4, y) || !digits(s + 4, 2, m) || !digits(s + 6, 2, d)) return false;
  if (m < 1 || m > 12 || d < 1 || d > 31) return false;
  out.day = daysFromCivil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
  if (len >= 13 && s[8] == 'T') {
    int hh, mm;
    if (!digits(s + 9, 2, hh) || !digits(s + 11, 2, mm) || hh > 23 || mm > 59) return false;
    out.minute = static_cast<int16_t>(hh * 60 + mm);
    out.utc = s[len - 1] == 'Z';
  }
  return true;
}

void utcIdentity(int64_t epochSec, int32_t& day, int16_t& minute) {
  int64_t d = epochSec / 86400;
  int64_t rem = epochSec % 86400;
  if (rem < 0) {
    rem += 86400;
    d -= 1;
  }
  day = static_cast<int32_t>(d);
  minute = static_cast<int16_t>(rem / 60);
}

void formatUtcStamp(int64_t epochSec, char* out, size_t outSize) {
  int32_t day;
  int16_t minute;
  utcIdentity(epochSec, day, minute);
  int y;
  unsigned m, d;
  civilFromDays(day, y, m, d);
  const int sec = static_cast<int>(((epochSec % 60) + 60) % 60);
  snprintf(out, outSize, "%04d%02u%02uT%02d%02d%02dZ", y, m, d, minute / 60, minute % 60, sec);
}

void formatDate(int32_t day, char* out, size_t outSize) {
  int y;
  unsigned m, d;
  civilFromDays(day, y, m, d);
  snprintf(out, outSize, "%04d%02u%02u", y, m, d);
}

}  // namespace ics
