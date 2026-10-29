#include "../src/hardware/civil_time.h"

#include <cstdio>

static int failures = 0;

static void expect(bool ok, const char* name) {
  if (ok) return;
  std::fprintf(stderr, "FAIL %s\n", name);
  failures += 1;
}

int main() {
  expect(civilDaysInMonth(2023, 2) == 28, "2023 is not a leap year");
  expect(civilDaysInMonth(2024, 2) == 29, "2024 is a leap year");
  expect(civilDaysInMonth(1900, 2) == 28, "1900 is not a leap year");
  expect(civilDaysInMonth(2000, 2) == 29, "2000 is a leap year");
  expect(civilDaysInMonth(2026, 10) == 31, "October has 31 days");
  expect(civilDaysInMonth(2026, 13) == 0, "month 13 is empty");

  CivilTime epoch;
  civilFromUnix(0, 0, epoch);
  expect(epoch.year == 1970 && epoch.month == 1 && epoch.day == 1, "epoch date");
  expect(epoch.hour == 0 && epoch.minute == 0 && epoch.second == 0, "epoch time");
  expect(epoch.wday == 4, "epoch was Thursday");

  // 2026-10-29 00:00:00 UTC. That Thursday is the day this panel work started.
  CivilTime today;
  civilFromUnix(1793232000, 0, today);
  expect(today.year == 2026 && today.month == 10 && today.day == 29, "2026-10-29");
  expect(today.wday == 4, "2026-10-29 is Thursday");
  expect(civilWeekday(20755) == 4, "weekday of that day count");

  CivilTime first;
  civilFromUnix(1793232000 - 28 * 86400, 0, first);
  expect(first.day == 1 && first.wday == 4, "October 2026 starts on Thursday");
  expect(civilWeekdayOf(2026, 10, 1) == 4, "first of October 2026 is column Thursday");
  expect(civilDaysFromYmd(1970, 1, 1) == 0, "epoch day count");

  CivilTime shifted;
  civilFromUnix(1793232000 + 30 * 60, -60, shifted);
  expect(shifted.year == 2026 && shifted.month == 10 && shifted.day == 28, "offset crosses midnight");
  expect(shifted.hour == 23 && shifted.minute == 30, "offset minutes");

  CivilTime leap;
  // 2024-02-29 12:00:00 UTC = 1709212800? Compute from days.
  // Verified by the conversion itself against a known unix: 1709208000 is
  // 2024-02-29 12:00:00 UTC.
  civilFromUnix(1709208000, 0, leap);
  expect(leap.year == 2024 && leap.month == 2 && leap.day == 29, "leap day");
  expect(leap.hour == 12, "leap day noon");

  if (failures != 0) {
    std::fprintf(stderr, "%d failed\n", failures);
    return 1;
  }
  std::printf("civil time ok\n");
  return 0;
}
