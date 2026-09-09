#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <memory>

#include "FlashcardDeck.h"
#include "FlashcardScheduler.h"
#include "activities/Activity.h"

class FlashcardsActivity final : public Activity {
 public:
  explicit FlashcardsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Flashcards", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Screen : uint8_t { NoClock, NoDeck, Overview, Question, Answer, Done };

  static constexpr const char* DECK_PATH = "/apps/flashcards/gre.deck";
  static constexpr const char* PROGRESS_PATH = "/apps/flashcards/gre.prog";
  static constexpr uint8_t RATINGS_PER_SAVE = 10;
  static constexpr uint8_t NEW_PER_DAY_STEP = 5;

  Screen screen = Screen::Overview;
  FlashcardDeck deck;
  FlashcardScheduler scheduler;
  std::unique_ptr<uint8_t[]> payload;
  FlashcardDeck::Card card;
  uint16_t currentIndex = FlashcardScheduler::INVALID_INDEX;
  uint8_t selectedRating = static_cast<uint8_t>(FlashcardScheduler::Rating::Good);
  uint8_t ratingsSinceSave = 0;

  bool loadResources();
  void showNextCard();
  void commitRating();
  void persist();
  void syncClock();

  void renderOverview();
  void renderQuestion();
  void renderAnswer();
  void renderDone();
  void renderMessage(const char* title, const char* hint);
  void renderStageSubtitle(char* buf, size_t bufSize) const;
  void renderCounters(int y);
  int drawParagraph(int fontId, EpdFontFamily::Style style, int x, int y, int width, int bottomLimit, const char* text);
  static void formatInterval(char* buf, size_t bufSize, uint32_t seconds);
};
