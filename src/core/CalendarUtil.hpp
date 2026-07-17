#pragma once

namespace vsm {

constexpr bool is_leap_year(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

// Proleptic Gregorian validity for a full wall-clock tuple. Shared by backup-name parsing and
// save-slot metadata so the two never disagree on leap years or month lengths.
constexpr bool is_valid_calendar_datetime(int year, int month, int day, int hour, int minute,
                                          int second) {
  if (year < 1 || year > 9999 || month < 1 || month > 12 || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 || second < 0 || second > 59) {
    return false;
  }
  constexpr int kMonthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int days = kMonthDays[month - 1];
  if (month == 2 && is_leap_year(year)) {
    ++days;
  }
  return day >= 1 && day <= days;
}

} // namespace vsm
