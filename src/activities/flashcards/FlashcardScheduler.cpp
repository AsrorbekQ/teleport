#include "FlashcardScheduler.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "util/DateUtils.h"

namespace {
constexpr const char* TAG = "FCSCHED";
constexpr uint32_t LEARNING_STEPS[] = {60, 600};  // seconds, as Anki's default 1m / 10m
constexpr uint8_t LEARNING_STEP_COUNT = sizeof(LEARNING_STEPS) / sizeof(LEARNING_STEPS[0]);
constexpr uint32_t RELEARNING_STEP = 600;
constexpr uint16_t GRADUATING_INTERVAL = 1;
constexpr uint16_t EASY_INTERVAL = 4;
constexpr uint16_t MIN_EASE = 1300;
constexpr uint16_t MAX_EASE = 5000;
constexpr uint32_t MAX_INTERVAL = 36500;
constexpr uint32_t LEARN_AHEAD_SECONDS = 1200;
}  // namespace

bool FlashcardScheduler::init(uint16_t cardCount, const char* progressPath) {
  release();
  states = makeUniqueNoThrow<CardState[]>(cardCount);
  if (!states) {
    LOG_ERR(TAG, "OOM: %u card states", cardCount);
    return false;
  }
  count = cardCount;
  strncpy(path, progressPath, sizeof(path) - 1);

  HalFile file;
  if (Storage.openFileForRead(TAG, path, file)) {
    Header header{};
    const size_t stateBytes = static_cast<size_t>(count) * sizeof(CardState);
    if (file.read(&header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
        memcmp(header.magic, "CPFP", 4) == 0 && header.version == FILE_VERSION && header.cardCount == count &&
        file.read(states.get(), stateBytes) == static_cast<int>(stateBytes)) {
      lastStudyDay = header.lastStudyDay;
      newTodayCount = header.newToday;
      reviewedTodayCount = header.reviewedToday;
      newPerDayLimit = std::clamp(header.newPerDay, MIN_NEW_PER_DAY, MAX_NEW_PER_DAY);
      LOG_INF(TAG, "Loaded progress for %u cards", count);
    } else {
      LOG_ERR(TAG, "Progress file mismatch, starting fresh");
      for (uint16_t i = 0; i < count; i++) states[i] = CardState{};
    }
  }
  isDirty = false;
  rollDayIfNeeded();
  return true;
}

bool FlashcardScheduler::save() {
  if (!states) return false;
  HalFile file;
  if (!Storage.openFileForWrite(TAG, path, file)) {
    LOG_ERR(TAG, "Cannot write %s", path);
    return false;
  }
  Header header{};
  memcpy(header.magic, "CPFP", 4);
  header.version = FILE_VERSION;
  header.newPerDay = newPerDayLimit;
  header.cardCount = count;
  header.lastStudyDay = lastStudyDay;
  header.newToday = newTodayCount;
  header.reviewedToday = reviewedTodayCount;
  const size_t stateBytes = static_cast<size_t>(count) * sizeof(CardState);
  if (file.write(&header, sizeof(header)) != sizeof(header) || file.write(states.get(), stateBytes) != stateBytes) {
    LOG_ERR(TAG, "Short write to %s", path);
    return false;
  }
  isDirty = false;
  return true;
}

void FlashcardScheduler::release() {
  states.reset();
  count = 0;
}

void FlashcardScheduler::rollDayIfNeeded() {
  const uint32_t today = DateUtils::todayIndex();
  if (today != lastStudyDay) {
    lastStudyDay = today;
    newTodayCount = 0;
    reviewedTodayCount = 0;
    isDirty = true;
  }
}

uint32_t FlashcardScheduler::learningStepSeconds(uint8_t step) {
  return LEARNING_STEPS[std::min<uint8_t>(step, LEARNING_STEP_COUNT - 1)];
}

uint32_t FlashcardScheduler::nextReviewInterval(const CardState& s, Rating rating) {
  const uint32_t current = std::max<uint32_t>(s.interval, 1);
  uint32_t next = current + 1;
  switch (rating) {
    case Rating::Hard:
      next = std::max(next, current * 12 / 10);
      break;
    case Rating::Good:
      next = std::max(next, current * s.ease / 1000);
      break;
    case Rating::Easy:
      next = std::max(next, current * s.ease / 1000 * 13 / 10);
      break;
    case Rating::Again:
      next = 1;
      break;
  }
  return std::min(next, MAX_INTERVAL);
}

void FlashcardScheduler::applyRating(CardState& s, Rating rating, uint32_t now, uint32_t today) const {
  auto graduate = [&](uint32_t days) {
    s.stage = static_cast<uint8_t>(Stage::Review);
    s.step = 0;
    s.interval = static_cast<uint16_t>(std::min(days, MAX_INTERVAL));
    s.due = DateUtils::startOfLocalDayUtc(today + s.interval);
  };

  switch (static_cast<Stage>(s.stage)) {
    case Stage::New:
    case Stage::Learning:
      s.stage = static_cast<uint8_t>(Stage::Learning);
      switch (rating) {
        case Rating::Again:
          s.step = 0;
          s.due = now + learningStepSeconds(0);
          break;
        case Rating::Hard:
          s.due = now + (s.step == 0 ? (LEARNING_STEPS[0] + learningStepSeconds(1)) / 2 : learningStepSeconds(s.step));
          break;
        case Rating::Good:
          if (s.step + 1 >= LEARNING_STEP_COUNT) {
            graduate(GRADUATING_INTERVAL);
          } else {
            s.step++;
            s.due = now + learningStepSeconds(s.step);
          }
          break;
        case Rating::Easy:
          graduate(EASY_INTERVAL);
          break;
      }
      break;

    case Stage::Review:
      if (rating == Rating::Again) {
        s.lapses = static_cast<uint8_t>(std::min<int>(s.lapses + 1, 255));
        s.ease = static_cast<uint16_t>(std::max<int>(MIN_EASE, s.ease - 200));
        s.stage = static_cast<uint8_t>(Stage::Relearning);
        s.step = 0;
        s.interval = 1;
        s.due = now + RELEARNING_STEP;
      } else {
        const uint32_t next = nextReviewInterval(s, rating);
        if (rating == Rating::Hard) s.ease = static_cast<uint16_t>(std::max<int>(MIN_EASE, s.ease - 150));
        if (rating == Rating::Easy) s.ease = static_cast<uint16_t>(std::min<int>(MAX_EASE, s.ease + 150));
        graduate(next);
      }
      break;

    case Stage::Relearning:
      switch (rating) {
        case Rating::Again:
        case Rating::Hard:
          s.step = 0;
          s.due = now + RELEARNING_STEP;
          break;
        case Rating::Good:
          graduate(s.interval);
          break;
        case Rating::Easy:
          graduate(std::max<uint32_t>(s.interval, 2));
          break;
      }
      break;
  }
  s.reps = static_cast<uint8_t>(std::min<int>(s.reps + 1, 255));
}

void FlashcardScheduler::rate(uint16_t index, Rating rating) {
  if (!states || index >= count) return;
  rollDayIfNeeded();
  CardState& s = states[index];
  if (static_cast<Stage>(s.stage) == Stage::New) newTodayCount++;
  reviewedTodayCount++;
  applyRating(s, rating, DateUtils::nowUtc(), DateUtils::todayIndex());
  isDirty = true;
}

uint32_t FlashcardScheduler::previewIntervalSeconds(uint16_t index, Rating rating) const {
  if (!states || index >= count) return 0;
  CardState copy = states[index];
  const uint32_t now = DateUtils::nowUtc();
  applyRating(copy, rating, now, DateUtils::todayIndex());
  if (static_cast<Stage>(copy.stage) == Stage::Review) {
    return static_cast<uint32_t>(copy.interval) * DateUtils::SECONDS_PER_DAY;
  }
  return copy.due > now ? copy.due - now : 0;
}

uint16_t FlashcardScheduler::nextCard() {
  if (!states) return INVALID_INDEX;
  rollDayIfNeeded();
  const uint32_t now = DateUtils::nowUtc();

  uint16_t bestLearning = INVALID_INDEX;
  uint16_t bestReview = INVALID_INDEX;
  uint16_t firstNew = INVALID_INDEX;
  uint16_t aheadLearning = INVALID_INDEX;
  for (uint16_t i = 0; i < count; i++) {
    const CardState& s = states[i];
    switch (static_cast<Stage>(s.stage)) {
      case Stage::Learning:
      case Stage::Relearning:
        if (s.due <= now) {
          if (bestLearning == INVALID_INDEX || s.due < states[bestLearning].due) bestLearning = i;
        } else if (s.due <= now + LEARN_AHEAD_SECONDS) {
          if (aheadLearning == INVALID_INDEX || s.due < states[aheadLearning].due) aheadLearning = i;
        }
        break;
      case Stage::Review:
        if (s.due <= now && (bestReview == INVALID_INDEX || s.due < states[bestReview].due)) bestReview = i;
        break;
      case Stage::New:
        if (firstNew == INVALID_INDEX) firstNew = i;
        break;
    }
  }

  if (bestLearning != INVALID_INDEX) return bestLearning;
  if (bestReview != INVALID_INDEX && reviewedTodayCount < MAX_REVIEWS_PER_DAY) return bestReview;
  if (firstNew != INVALID_INDEX && newTodayCount < newPerDayLimit) return firstNew;
  return aheadLearning;
}

FlashcardScheduler::Counts FlashcardScheduler::counts() const {
  Counts c;
  if (!states) return c;
  const uint32_t now = DateUtils::nowUtc();
  uint16_t newCards = 0;
  for (uint16_t i = 0; i < count; i++) {
    const CardState& s = states[i];
    switch (static_cast<Stage>(s.stage)) {
      case Stage::New:
        newCards++;
        break;
      case Stage::Learning:
      case Stage::Relearning:
        if (s.due <= now) c.learningDue++;
        break;
      case Stage::Review:
        c.learned++;
        if (s.due <= now) c.reviewDue++;
        break;
    }
  }
  const uint16_t allowance = newTodayCount < newPerDayLimit ? newPerDayLimit - newTodayCount : 0;
  c.newAvailable = std::min(newCards, allowance);
  return c;
}

uint32_t FlashcardScheduler::secondsUntilNextDue() const {
  if (!states) return UINT32_MAX;
  const uint32_t now = DateUtils::nowUtc();
  uint32_t best = UINT32_MAX;
  for (uint16_t i = 0; i < count; i++) {
    const CardState& s = states[i];
    if (static_cast<Stage>(s.stage) == Stage::New) continue;
    if (s.due <= now) return 0;
    best = std::min(best, s.due - now);
  }
  return best;
}

void FlashcardScheduler::setNewPerDay(uint8_t value) {
  const uint8_t clamped = std::clamp(value, MIN_NEW_PER_DAY, MAX_NEW_PER_DAY);
  if (clamped == newPerDayLimit) return;
  newPerDayLimit = clamped;
  isDirty = true;
}
