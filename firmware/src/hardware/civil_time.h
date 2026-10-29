#pragma once

#include <stdint.h>

// Civil date from a Unix second and a local offset.
// offsetMin is minutes to add to UTC (east of UTC is positive). That is the
// opposite of JavaScript's getTimezoneOffset().

struct CivilTime {
  int year = 1970;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int wday = 4;  // 0 Sunday. 1970-01-01 was Thursday.
};

inline bool civilIsLeap(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

inline int civilDaysInMonth(int year, int month) {
  static const int kDays[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && civilIsLeap(year)) return 29;
  return kDays[month];
}

// Days since 1970-01-01. Sunday is 0.
// Days since 1970-01-01 for a civil date.
inline int64_t civilDaysFromYmd(int year, int month, int day) {
  year -= month <= 2 ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

inline int civilWeekday(int64_t daysSinceEpoch) {
  int wday = static_cast<int>((daysSinceEpoch + 4) % 7);
  if (wday < 0) wday += 7;
  return wday;
}

inline int civilWeekdayOf(int year, int month, int day) {
  return civilWeekday(civilDaysFromYmd(year, month, day));
}

// Howard Hinnant's civil_from_days. z is days since 1970-01-01.
inline void civilFromDays(int64_t z, int& year, int& month, int& day) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  year = y + (month <= 2 ? 1 : 0);
}

inline void civilFromUnix(int64_t unixSeconds, int offsetMin, CivilTime& out) {
  int64_t local = unixSeconds + static_cast<int64_t>(offsetMin) * 60;
  int64_t days = local / 86400;
  int64_t sod = local % 86400;
  if (sod < 0) {
    sod += 86400;
    days -= 1;
  }
  out.hour = static_cast<int>(sod / 3600);
  out.minute = static_cast<int>((sod % 3600) / 60);
  out.second = static_cast<int>(sod % 60);
  out.wday = civilWeekday(days);
  civilFromDays(days, out.year, out.month, out.day);
}
