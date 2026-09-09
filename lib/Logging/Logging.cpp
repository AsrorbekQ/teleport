#include "Logging.h"

#include <esp_log.h>

#include <cstdarg>
#include <string>

#define MAX_ENTRY_LEN 256
#define MAX_LOG_LINES 16

// Simple ring buffer log, useful for error reporting when we encounter a crash
RTC_NOINIT_ATTR char logMessages[MAX_LOG_LINES][MAX_ENTRY_LEN];
RTC_NOINIT_ATTR size_t logHead = 0;
// Magic word written alongside logHead to detect uninitialized RTC memory.
// RTC_NOINIT_ATTR is not zeroed on cold boot, so logHead may appear in-range
// (0..MAX_LOG_LINES-1) by chance even though logMessages is garbage. The magic
// value is only set by clearLastLogs(), so its absence means the buffer was
// never properly initialized.
RTC_NOINIT_ATTR uint32_t rtcLogMagic;
static constexpr uint32_t LOG_RTC_MAGIC = 0xDEADBEEF;

// DRAM ring for on-device diagnostics: the USB port is not usable for serial on every
// unit, so apps can dump this to the SD card. ~6 KB static, never reallocated.
#define RECENT_LOG_LINES 40
#define RECENT_ENTRY_LEN 160
static char recentLogs[RECENT_LOG_LINES][RECENT_ENTRY_LEN];
static size_t recentHead = 0;

static void addToRecentLogs(const char* message) {
  strncpy(recentLogs[recentHead], message, RECENT_ENTRY_LEN - 1);
  recentLogs[recentHead][RECENT_ENTRY_LEN - 1] = '\0';
  recentHead = (recentHead + 1) % RECENT_LOG_LINES;
}

std::string getRecentLogs() {
  std::string output;
  output.reserve(RECENT_LOG_LINES * 64);
  for (size_t i = 0; i < RECENT_LOG_LINES; i++) {
    const size_t idx = (recentHead + i) % RECENT_LOG_LINES;
    if (recentLogs[idx][0] != '\0') {
      output.append(recentLogs[idx], strnlen(recentLogs[idx], RECENT_ENTRY_LEN));
      if (output.back() != '\n') output.push_back('\n');
    }
  }
  return output;
}

static vprintf_like_t previousEspLogHandler = nullptr;

static int espLogToRing(const char* format, va_list args) {
  char buf[RECENT_ENTRY_LEN];
  vsnprintf(buf, sizeof(buf), format, args);
  addToRecentLogs(buf);
  if (logSerial) logSerial.print(buf);
  return 0;
}

void installEspLogHook() { previousEspLogHandler = esp_log_set_vprintf(espLogToRing); }

void addToLogRingBuffer(const char* message) {
  addToRecentLogs(message);
  // Add the message to the ring buffer, overwriting old messages if necessary.
  // If the magic is wrong or logHead is out of range (RTC_NOINIT_ATTR garbage
  // on cold boot), clear the entire buffer so subsequent reads are safe.
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    memset(logMessages, 0, sizeof(logMessages));
    logHead = 0;
    rtcLogMagic = LOG_RTC_MAGIC;
  }
  strncpy(logMessages[logHead], message, MAX_ENTRY_LEN - 1);
  logMessages[logHead][MAX_ENTRY_LEN - 1] = '\0';
  logHead = (logHead + 1) % MAX_LOG_LINES;
}

// Since logging can take a large amount of flash, we want to make the format string as short as possible.
// This logPrintf prepend the timestamp, level and origin to the user-provided message, so that the user only needs to
// provide the format string for the message itself.
void logPrintf(const char* level, const char* origin, const char* format, ...) {
  va_list args;
  va_start(args, format);
  char buf[MAX_ENTRY_LEN];
  char* c = buf;
  // add timestamp, level and origin
  {
    unsigned long ms = millis();
    int len = snprintf(c, sizeof(buf), "[%lu] [%s] [%s] ", ms, level, origin);
    // error while writing => return
    if (len < 0) {
      va_end(args);
      return;
    }
    // clamp c to be in buffer range
    c += std::min(len, MAX_ENTRY_LEN);
  }
  // add the user message
  {
    int len = vsnprintf(c, sizeof(buf) - (c - buf), format, args);
    if (len < 0) {
      va_end(args);
      return;
    }
  }
  va_end(args);
  if (logSerial) {
    logSerial.print(buf);
  }
  addToLogRingBuffer(buf);
}

std::string getLastLogs() {
  if (rtcLogMagic != LOG_RTC_MAGIC) {
    return {};
  }
  std::string output;
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    size_t idx = (logHead + i) % MAX_LOG_LINES;
    if (logMessages[idx][0] != '\0') {
      const size_t len = strnlen(logMessages[idx], MAX_ENTRY_LEN);
      output.append(logMessages[idx], len);
    }
  }
  return output;
}

// Checks whether the RTC log state is consistent: rtcLogMagic must equal
// LOG_RTC_MAGIC and logHead must be in 0..MAX_LOG_LINES-1. Returns true if
// corruption is detected, in which case rtcLogMagic is still invalid and
// logMessages may contain garbage. Callers (e.g. HalSystem::begin on the
// panic-reboot path) must call clearLastLogs() after a true result to fully
// reinitialize the ring buffer and stamp the magic before getLastLogs() is used.
bool sanitizeLogHead() {
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    logHead = 0;
    return true;
  }
  return false;
}

void clearLastLogs() {
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    logMessages[i][0] = '\0';
  }
  logHead = 0;
  rtcLogMagic = LOG_RTC_MAGIC;
}
