#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"

// System time is UTC (set by NTP). Local time applies the user's clock offset setting.
namespace DateUtils {

constexpr int32_t SECONDS_PER_DAY = 86400;

inline bool hasValidTime() {
  const time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  return t.tm_year >= (2020 - 1900);
}

inline int32_t utcOffsetSeconds() { return (static_cast<int32_t>(SETTINGS.clockUtcOffsetQ) - 48) * 15 * 60; }

inline uint32_t nowUtc() { return static_cast<uint32_t>(time(nullptr)); }

// Days since epoch in the user's local time zone.
inline uint32_t todayIndex() {
  return static_cast<uint32_t>((static_cast<int64_t>(nowUtc()) + utcOffsetSeconds()) / SECONDS_PER_DAY);
}

// UTC timestamp of local midnight that starts the given local day index.
inline uint32_t startOfLocalDayUtc(uint32_t dayIndex) {
  return static_cast<uint32_t>(static_cast<int64_t>(dayIndex) * SECONDS_PER_DAY - utcOffsetSeconds());
}

inline void formatDay(char* buf, size_t bufSize, uint32_t dayIndex) {
  const time_t t = static_cast<time_t>(dayIndex) * SECONDS_PER_DAY;
  struct tm tm;
  gmtime_r(&t, &tm);
  snprintf(buf, bufSize, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

}  // namespace DateUtils
