#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// SM-2 scheduling in the style of Anki's classic scheduler: new cards pass through
// short learning steps, graduate to day-based review intervals scaled by an ease
// factor, and lapse back into relearning on "Again".
class FlashcardScheduler {
 public:
  enum class Stage : uint8_t { New = 0, Learning = 1, Review = 2, Relearning = 3 };
  enum class Rating : uint8_t { Again = 0, Hard = 1, Good = 2, Easy = 3 };

  struct CardState {
    uint32_t due = 0;       // UTC seconds; 0 for new cards
    uint16_t interval = 0;  // review interval in days
    uint16_t ease = 2500;   // permille, Anki style (2500 = 250%)
    uint8_t stage = 0;      // Stage
    uint8_t step = 0;       // learning / relearning step index
    uint8_t lapses = 0;
    uint8_t reps = 0;
  };

  struct Counts {
    uint16_t newAvailable = 0;  // new cards still allowed today
    uint16_t learningDue = 0;
    uint16_t reviewDue = 0;
    uint16_t learned = 0;  // cards that have graduated to review
  };

  static constexpr uint8_t FILE_VERSION = 1;
  static constexpr uint8_t DEFAULT_NEW_PER_DAY = 20;
  static constexpr uint8_t MIN_NEW_PER_DAY = 5;
  static constexpr uint8_t MAX_NEW_PER_DAY = 100;
  static constexpr uint16_t MAX_REVIEWS_PER_DAY = 200;
  static constexpr uint16_t INVALID_INDEX = 0xFFFF;

  // Allocates cardCount states and loads the progress file if it matches the deck.
  bool init(uint16_t cardCount, const char* progressPath);
  bool save();
  void release();

  uint16_t nextCard();  // INVALID_INDEX when nothing is due now
  void rate(uint16_t index, Rating rating);
  uint32_t previewIntervalSeconds(uint16_t index, Rating rating) const;
  Stage stageOf(uint16_t index) const { return static_cast<Stage>(states[index].stage); }

  Counts counts() const;
  uint16_t reviewedToday() const { return reviewedTodayCount; }
  uint8_t newPerDay() const { return newPerDayLimit; }
  void setNewPerDay(uint8_t value);
  // Seconds until the earliest pending learning/review card, 0 when something is due now.
  uint32_t secondsUntilNextDue() const;
  bool dirty() const { return isDirty; }

 private:
  struct Header {
    char magic[4];
    uint8_t version;
    uint8_t newPerDay;
    uint16_t cardCount;
    uint32_t lastStudyDay;
    uint16_t newToday;
    uint16_t reviewedToday;
  };

  std::unique_ptr<CardState[]> states;
  uint16_t count = 0;
  char path[64] = {0};
  uint32_t lastStudyDay = 0;
  uint16_t newTodayCount = 0;
  uint16_t reviewedTodayCount = 0;
  uint8_t newPerDayLimit = DEFAULT_NEW_PER_DAY;
  bool isDirty = false;

  void rollDayIfNeeded();
  void applyRating(CardState& s, Rating rating, uint32_t now, uint32_t today) const;
  static uint32_t learningStepSeconds(uint8_t step);
  static uint32_t nextReviewInterval(const CardState& s, Rating rating);
};
