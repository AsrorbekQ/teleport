#include "HabitStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr const char* TAG = "HABITS";
constexpr uint16_t SHIFT_HEADROOM = 100;  // days of slack after a window shift

struct Header {
  char magic[4];
  uint8_t version;
  uint8_t count;
  uint16_t reserved;
  uint32_t windowStart;
};

bool getBit(const uint8_t* bits, uint16_t i) { return (bits[i / 8] >> (i % 8)) & 1; }
void setBit(uint8_t* bits, uint16_t i, bool value) {
  if (value) {
    bits[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
  } else {
    bits[i / 8] &= static_cast<uint8_t>(~(1u << (i % 8)));
  }
}
}  // namespace

bool HabitStore::load(const char* storePath) {
  strncpy(path, storePath, sizeof(path) - 1);
  habitCount = 0;
  windowStart = 0;
  memset(habits, 0, sizeof(habits));

  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) {
    LOG_INF(TAG, "No habit file yet");
    return true;
  }
  Header header{};
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || memcmp(header.magic, "CPHB", 4) != 0 ||
      header.version != FILE_VERSION || header.count > MAX_HABITS) {
    LOG_ERR(TAG, "Bad habit file, starting empty");
    return false;
  }
  const size_t bytes = static_cast<size_t>(header.count) * sizeof(Habit);
  if (file.read(habits, bytes) != static_cast<int>(bytes)) {
    LOG_ERR(TAG, "Short habit file, starting empty");
    memset(habits, 0, sizeof(habits));
    return false;
  }
  habitCount = header.count;
  windowStart = header.windowStart;
  for (uint8_t i = 0; i < habitCount; i++) habits[i].name[NAME_LENGTH - 1] = 0;
  return true;
}

bool HabitStore::save() {
  HalFile file;
  if (!Storage.openFileForWrite(TAG, path, file)) {
    LOG_ERR(TAG, "Cannot write %s", path);
    return false;
  }
  Header header{};
  memcpy(header.magic, "CPHB", 4);
  header.version = FILE_VERSION;
  header.count = habitCount;
  header.windowStart = windowStart;
  const size_t bytes = static_cast<size_t>(habitCount) * sizeof(Habit);
  if (file.write(&header, sizeof(header)) != sizeof(header) || file.write(habits, bytes) != bytes) {
    LOG_ERR(TAG, "Short write to %s", path);
    return false;
  }
  return true;
}

bool HabitStore::add(const char* newName) {
  if (habitCount >= MAX_HABITS || !newName || !*newName) return false;
  Habit& habit = habits[habitCount];
  memset(&habit, 0, sizeof(habit));
  strncpy(habit.name, newName, NAME_LENGTH - 1);
  habitCount++;
  return true;
}

void HabitStore::remove(uint8_t index) {
  if (index >= habitCount) return;
  for (uint8_t i = index; i + 1 < habitCount; i++) habits[i] = habits[i + 1];
  habitCount--;
  memset(&habits[habitCount], 0, sizeof(Habit));
}

int32_t HabitStore::slotFor(uint32_t day) const {
  if (windowStart == 0 || day < windowStart || day - windowStart >= WINDOW_DAYS) return -1;
  return static_cast<int32_t>(day - windowStart);
}

void HabitStore::ensureWindowCovers(uint32_t day) {
  if (windowStart == 0) {
    // Fresh store: leave room before today so backfilled or timezone-shifted days fit.
    windowStart = day > WINDOW_DAYS / 2 ? day - WINDOW_DAYS / 2 : 0;
    return;
  }
  if (day < windowStart) {
    // Move the window back (bits shift later). A jump of months back means a wrong clock, so keep history.
    const uint32_t shift = windowStart - day + SHIFT_HEADROOM;
    if (shift > WINDOW_DAYS / 2) {
      LOG_ERR(TAG, "Day %lu is far before window start %lu, ignoring", static_cast<unsigned long>(day),
              static_cast<unsigned long>(windowStart));
      return;
    }
    for (uint8_t h = 0; h < habitCount; h++) {
      uint8_t shifted[WINDOW_DAYS / 8] = {0};
      for (uint32_t i = 0; i + shift < WINDOW_DAYS; i++) {
        if (getBit(habits[h].days, static_cast<uint16_t>(i))) setBit(shifted, static_cast<uint16_t>(i + shift), true);
      }
      memcpy(habits[h].days, shifted, sizeof(shifted));
    }
    windowStart -= shift;
    return;
  }
  if (day - windowStart < WINDOW_DAYS) return;

  const uint32_t shift = day - windowStart - (WINDOW_DAYS - 1) + SHIFT_HEADROOM;
  for (uint8_t h = 0; h < habitCount; h++) {
    uint8_t shifted[WINDOW_DAYS / 8] = {0};
    for (uint32_t i = shift; i < WINDOW_DAYS; i++) {
      if (getBit(habits[h].days, static_cast<uint16_t>(i))) setBit(shifted, static_cast<uint16_t>(i - shift), true);
    }
    memcpy(habits[h].days, shifted, sizeof(shifted));
  }
  windowStart += shift;
}

bool HabitStore::isDone(uint8_t index, uint32_t day) const {
  if (index >= habitCount) return false;
  const int32_t slot = slotFor(day);
  return slot >= 0 && getBit(habits[index].days, static_cast<uint16_t>(slot));
}

void HabitStore::setDone(uint8_t index, uint32_t day, bool done) {
  if (index >= habitCount) return;
  ensureWindowCovers(day);
  const int32_t slot = slotFor(day);
  if (slot < 0) return;  // older than the window: history that far back is not tracked
  setBit(habits[index].days, static_cast<uint16_t>(slot), done);
}

uint16_t HabitStore::currentStreak(uint8_t index, uint32_t today) const {
  if (index >= habitCount) return 0;
  // An unfinished today does not break the streak; count back from yesterday instead.
  uint32_t day = isDone(index, today) ? today : today - 1;
  uint16_t streak = 0;
  while (day > 0 && isDone(index, day)) {
    streak++;
    day--;
  }
  return streak;
}

uint16_t HabitStore::bestStreak(uint8_t index, uint32_t today) const {
  if (index >= habitCount || windowStart == 0) return 0;
  uint16_t best = 0;
  uint16_t run = 0;
  for (uint32_t day = windowStart; day <= today; day++) {
    if (isDone(index, day)) {
      run++;
      best = std::max(best, run);
    } else {
      run = 0;
    }
  }
  return best;
}

uint16_t HabitStore::doneInLast(uint8_t index, uint32_t today, uint16_t days) const {
  if (index >= habitCount) return 0;
  uint16_t done = 0;
  for (uint16_t i = 0; i < days && i <= today; i++) {
    if (isDone(index, today - i)) done++;
  }
  return done;
}

uint16_t HabitStore::total(uint8_t index) const {
  if (index >= habitCount) return 0;
  uint16_t done = 0;
  for (uint16_t i = 0; i < WINDOW_DAYS; i++) {
    if (getBit(habits[index].days, i)) done++;
  }
  return done;
}
