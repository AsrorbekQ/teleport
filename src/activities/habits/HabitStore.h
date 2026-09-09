#pragma once

#include <cstddef>
#include <cstdint>

// Fixed-size habit list with a 400-day completion bitmap per habit.
// Everything lives inline (under 1 KB) so the activity needs no heap allocations.
class HabitStore {
 public:
  static constexpr uint8_t MAX_HABITS = 12;
  static constexpr uint8_t NAME_LENGTH = 32;  // including terminator
  static constexpr uint16_t WINDOW_DAYS = 400;
  static constexpr uint8_t FILE_VERSION = 1;

  bool load(const char* path);  // a missing file yields an empty store
  bool save();

  uint8_t count() const { return habitCount; }
  const char* name(uint8_t index) const { return habits[index].name; }
  bool add(const char* newName);
  void remove(uint8_t index);

  bool isDone(uint8_t index, uint32_t day) const;
  void setDone(uint8_t index, uint32_t day, bool done);

  uint16_t currentStreak(uint8_t index, uint32_t today) const;
  uint16_t bestStreak(uint8_t index, uint32_t today) const;
  uint16_t doneInLast(uint8_t index, uint32_t today, uint16_t days) const;
  uint16_t total(uint8_t index) const;

 private:
  struct Habit {
    char name[NAME_LENGTH];
    uint8_t days[WINDOW_DAYS / 8];
  };

  Habit habits[MAX_HABITS] = {};
  uint8_t habitCount = 0;
  uint32_t windowStart = 0;  // day index stored in bit 0
  char path[64] = {0};

  int32_t slotFor(uint32_t day) const;
  void ensureWindowCovers(uint32_t day);
};
