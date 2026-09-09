#include "FlashcardsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/util/WifiConnectHelper.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DateUtils.h"

namespace {
constexpr const char* TAG = "FLASH";
constexpr int SIDE_PADDING = 20;
constexpr int RATING_BOX_HEIGHT = 40;
constexpr int RATING_BAR_HEIGHT = RATING_BOX_HEIGHT + 24;
constexpr int RATING_GAP = 8;
constexpr uint8_t RATING_COUNT = 4;

using Rating = FlashcardScheduler::Rating;
using Stage = FlashcardScheduler::Stage;
}  // namespace

void FlashcardsActivity::onEnter() {
  Activity::onEnter();
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/flashcards");

  if (!DateUtils::hasValidTime()) {
    screen = Screen::NoClock;
  } else {
    loadResources();
  }
  requestUpdate();
}

void FlashcardsActivity::onExit() {
  persist();
  scheduler.release();
  deck.close();
  payload.reset();
  Activity::onExit();
}

bool FlashcardsActivity::loadResources() {
  if (!deck.open(DECK_PATH)) {
    screen = Screen::NoDeck;
    return false;
  }
  if (!payload) {
    payload = makeUniqueNoThrow<uint8_t[]>(FlashcardDeck::MAX_PAYLOAD);
    if (!payload) {
      LOG_ERR(TAG, "OOM: %u byte card buffer", static_cast<unsigned>(FlashcardDeck::MAX_PAYLOAD));
      deck.close();
      screen = Screen::NoDeck;
      return false;
    }
  }
  if (!scheduler.init(deck.count(), PROGRESS_PATH)) {
    deck.close();
    screen = Screen::NoDeck;
    return false;
  }
  screen = Screen::Overview;
  return true;
}

void FlashcardsActivity::persist() {
  if (scheduler.dirty()) {
    scheduler.save();
  }
  ratingsSinceSave = 0;
}

void FlashcardsActivity::syncClock() {
  ensureWifiConnected([this]() {
    {
      RenderLock lock;
      GUI.drawPopup(renderer, tr(STR_SYNCING_TIME));
      renderer.displayBuffer();
    }
    if (WifiConnectHelper::waitForTimeSync() && DateUtils::hasValidTime()) {
      loadResources();
    }
    requestUpdate();
  });
}

void FlashcardsActivity::showNextCard() {
  currentIndex = scheduler.nextCard();
  if (currentIndex == FlashcardScheduler::INVALID_INDEX) {
    screen = Screen::Done;
  } else if (!deck.loadCard(currentIndex, payload.get(), card)) {
    LOG_ERR(TAG, "Failed to load card %u", currentIndex);
    screen = Screen::Overview;
  } else {
    screen = Screen::Question;
  }
  requestUpdate();
}

void FlashcardsActivity::commitRating() {
  scheduler.rate(currentIndex, static_cast<Rating>(selectedRating));
  if (++ratingsSinceSave >= RATINGS_PER_SAVE) {
    persist();
  }
  showNextCard();
}

void FlashcardsActivity::loop() {
  using Button = MappedInputManager::Button;
  const bool back = mappedInput.wasReleased(Button::Back);
  const bool confirm = mappedInput.wasReleased(Button::Confirm);
  const bool left = mappedInput.wasReleased(Button::Left);
  const bool right = mappedInput.wasReleased(Button::Right);

  switch (screen) {
    case Screen::NoClock:
      if (back) {
        finish();
      } else if (confirm) {
        syncClock();
      }
      break;

    case Screen::NoDeck:
      if (back || confirm) finish();
      break;

    case Screen::Overview:
      if (back) {
        finish();
      } else if (confirm) {
        showNextCard();
      } else if (left) {
        scheduler.setNewPerDay(scheduler.newPerDay() - NEW_PER_DAY_STEP);
        requestUpdate();
      } else if (right) {
        scheduler.setNewPerDay(scheduler.newPerDay() + NEW_PER_DAY_STEP);
        requestUpdate();
      }
      break;

    case Screen::Question:
      if (back) {
        persist();
        screen = Screen::Overview;
        requestUpdate();
      } else if (confirm) {
        selectedRating = static_cast<uint8_t>(Rating::Good);
        screen = Screen::Answer;
        requestUpdate();
      }
      break;

    case Screen::Answer:
      if (back) {
        persist();
        screen = Screen::Overview;
        requestUpdate();
      } else if (confirm) {
        commitRating();
      } else if (left && selectedRating > 0) {
        selectedRating--;
        requestUpdate();
      } else if (right && selectedRating < RATING_COUNT - 1) {
        selectedRating++;
        requestUpdate();
      }
      break;

    case Screen::Done:
      if (back || confirm) {
        screen = Screen::Overview;
        requestUpdate();
      }
      break;
  }
}

void FlashcardsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (screen) {
    case Screen::NoClock:
      renderMessage(tr(STR_APP_CLOCK_NOT_SET), tr(STR_APP_CLOCK_HINT));
      break;
    case Screen::NoDeck:
      renderMessage(tr(STR_FC_DECK_MISSING), tr(STR_FC_DECK_MISSING_HINT));
      break;
    case Screen::Overview:
      renderOverview();
      break;
    case Screen::Question:
      renderQuestion();
      break;
    case Screen::Answer:
      renderAnswer();
      break;
    case Screen::Done:
      renderDone();
      break;
  }
  renderer.displayBuffer();
}

void FlashcardsActivity::renderMessage(const char* title, const char* hint) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FLASHCARDS));

  const int titleY = pageHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 6;
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, title, true, EpdFontFamily::BOLD);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = titleY + renderer.getLineHeight(UI_12_FONT_ID) + 12;
  const auto lines = renderer.wrappedText(UI_10_FONT_ID, hint, pageWidth - SIDE_PADDING * 2, 3);
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
    y += lineHeight;
  }

  const bool canSync = screen == Screen::NoClock;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), canSync ? tr(STR_CLOCK_SYNC) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardsActivity::renderOverview() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FLASHCARDS));

  const auto counts = scheduler.counts();
  const int rowHeight = metrics.listRowHeight;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int textInset = (rowHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  const int valueRight = pageWidth - SIDE_PADDING;

  struct Row {
    const char* label;
    unsigned value;
    unsigned total;  // 0 = show plain value
    bool bold;
  };
  const Row rows[] = {
      {tr(STR_FC_NEW), counts.newAvailable, 0, true},
      {tr(STR_FC_LEARNING), counts.learningDue, 0, true},
      {tr(STR_FC_TO_REVIEW), counts.reviewDue, 0, true},
      {tr(STR_FC_LEARNED), counts.learned, deck.count(), false},
      {tr(STR_FC_REVIEWED_TODAY), scheduler.reviewedToday(), 0, false},
      {tr(STR_FC_NEW_PER_DAY), scheduler.newPerDay(), 0, false},
  };

  int y = contentTop;
  char value[24];
  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    if (i == 3) y += metrics.verticalSpacing * 2;
    const auto style = rows[i].bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    renderer.drawText(UI_12_FONT_ID, SIDE_PADDING, y + textInset, rows[i].label, true, style);
    if (rows[i].total > 0) {
      snprintf(value, sizeof(value), "%u / %u", rows[i].value, rows[i].total);
    } else {
      snprintf(value, sizeof(value), "%u", rows[i].value);
    }
    const int valueWidth = renderer.getTextWidth(UI_12_FONT_ID, value, style);
    renderer.drawText(UI_12_FONT_ID, valueRight - valueWidth, y + textInset, value, true, style);
    y += rowHeight;
  }

  const bool nothingDue = counts.newAvailable == 0 && counts.learningDue == 0 && counts.reviewDue == 0;
  if (nothingDue) {
    y += metrics.verticalSpacing * 2;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_FC_NOTHING_DUE));
    const uint32_t wait = scheduler.secondsUntilNextDue();
    if (wait != UINT32_MAX && wait > 0) {
      char buf[48];
      char interval[16];
      formatInterval(interval, sizeof(interval), wait);
      snprintf(buf, sizeof(buf), "%s %s", tr(STR_FC_NEXT_DUE_IN), interval);
      renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_10_FONT_ID) + 4, buf);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_FC_STUDY), tr(STR_FC_FEWER_NEW), tr(STR_FC_MORE_NEW));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardsActivity::renderStageSubtitle(char* buf, size_t bufSize) const {
  const char* stageName = "";
  switch (scheduler.stageOf(currentIndex)) {
    case Stage::New:
      stageName = tr(STR_FC_NEW);
      break;
    case Stage::Learning:
    case Stage::Relearning:
      stageName = tr(STR_FC_LEARNING);
      break;
    case Stage::Review:
      stageName = tr(STR_FC_REVIEW);
      break;
  }
  if (card.setId == 0) {
    snprintf(buf, bufSize, "%s - %s", tr(STR_FC_DOUBLE_DUTY), stageName);
  } else {
    snprintf(buf, bufSize, "%s %u - %s", tr(STR_FC_SET), card.setId, stageName);
  }
}

void FlashcardsActivity::renderCounters(int y) {
  const auto counts = scheduler.counts();
  char buf[64];
  snprintf(buf, sizeof(buf), "%s %u  |  %s %u  |  %s %u", tr(STR_FC_NEW), counts.newAvailable, tr(STR_FC_LEARNING),
           counts.learningDue, tr(STR_FC_REVIEW), counts.reviewDue);
  renderer.drawCenteredText(SMALL_FONT_ID, y, buf);
}

void FlashcardsActivity::renderQuestion() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  char subtitle[48];
  renderStageSubtitle(subtitle, sizeof(subtitle));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FLASHCARDS), subtitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int wordFont = NOTOSERIF_18_FONT_ID;
  const auto lines = renderer.wrappedText(wordFont, card.fields[FlashcardDeck::WORD], pageWidth - SIDE_PADDING * 2, 2,
                                          EpdFontFamily::BOLD);
  const int lineHeight = renderer.getLineHeight(wordFont);
  int y = contentTop + (contentBottom - contentTop) / 2 - (static_cast<int>(lines.size()) * lineHeight) / 2 - 20;
  for (const auto& line : lines) {
    renderer.drawCenteredText(wordFont, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += lineHeight;
  }

  renderCounters(contentBottom - renderer.getLineHeight(SMALL_FONT_ID) - 4);

  const auto labels = mappedInput.mapLabels(tr(STR_FC_EXIT), tr(STR_FC_SHOW_ANSWER), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

int FlashcardsActivity::drawParagraph(int fontId, EpdFontFamily::Style style, int x, int y, int width, int bottomLimit,
                                      const char* text) {
  const int lineHeight = renderer.getLineHeight(fontId);
  const char* cursor = text;
  while (*cursor && y + lineHeight <= bottomLimit) {
    const char* newline = strchr(cursor, '\n');
    const size_t segmentLength = newline ? static_cast<size_t>(newline - cursor) : strlen(cursor);
    if (segmentLength > 0) {
      const std::string segment(cursor, segmentLength);
      const int maxLines = (bottomLimit - y) / lineHeight;
      const auto lines = renderer.wrappedText(fontId, segment.c_str(), width, maxLines, style);
      for (const auto& line : lines) {
        renderer.drawText(fontId, x, y, line.c_str(), true, style);
        y += lineHeight;
      }
    }
    if (!newline) break;
    cursor = newline + 1;
  }
  return y;
}

void FlashcardsActivity::renderAnswer() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  char subtitle[48];
  renderStageSubtitle(subtitle, sizeof(subtitle));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FLASHCARDS), subtitle);

  const int textWidth = pageWidth - SIDE_PADDING * 2;
  const int ratingTop = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - RATING_BAR_HEIGHT;
  const int bottomLimit = ratingTop - metrics.verticalSpacing;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  y = drawParagraph(NOTOSANS_16_FONT_ID, EpdFontFamily::BOLD, SIDE_PADDING, y, textWidth, bottomLimit,
                    card.fields[FlashcardDeck::WORD]);
  y += 4;
  renderer.fillRect(SIDE_PADDING, y, textWidth, 2, true);
  y += 10;

  y = drawParagraph(NOTOSANS_14_FONT_ID, EpdFontFamily::REGULAR, SIDE_PADDING, y, textWidth, bottomLimit,
                    card.fields[FlashcardDeck::DEFINITION_1]);
  if (*card.fields[FlashcardDeck::DEFINITION_2]) {
    y += 8;
    y = drawParagraph(NOTOSANS_12_FONT_ID, EpdFontFamily::REGULAR, SIDE_PADDING, y, textWidth, bottomLimit,
                      card.fields[FlashcardDeck::DEFINITION_2]);
  }
  if (*card.fields[FlashcardDeck::EXAMPLE]) {
    y += 8;
    y = drawParagraph(NOTOSANS_12_FONT_ID, EpdFontFamily::ITALIC, SIDE_PADDING, y, textWidth, bottomLimit,
                      card.fields[FlashcardDeck::EXAMPLE]);
  }
  if (*card.fields[FlashcardDeck::SYNONYMS]) {
    y += 8;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %s", tr(STR_FC_SYNONYMS), card.fields[FlashcardDeck::SYNONYMS]);
    y = drawParagraph(NOTOSANS_12_FONT_ID, EpdFontFamily::REGULAR, SIDE_PADDING, y, textWidth, bottomLimit, buf);
  }

  const char* ratingNames[RATING_COUNT] = {tr(STR_FC_AGAIN), tr(STR_FC_HARD), tr(STR_FC_GOOD), tr(STR_FC_EASY)};
  const int boxWidth = (textWidth - RATING_GAP * (RATING_COUNT - 1)) / RATING_COUNT;
  const int labelInset = (RATING_BOX_HEIGHT - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  for (uint8_t i = 0; i < RATING_COUNT; i++) {
    const int x = SIDE_PADDING + i * (boxWidth + RATING_GAP);
    const bool selected = i == selectedRating;
    if (selected) {
      renderer.fillRoundedRect(x, ratingTop, boxWidth, RATING_BOX_HEIGHT, 6, Color::Black);
    } else {
      renderer.drawRoundedRect(x, ratingTop, boxWidth, RATING_BOX_HEIGHT, 1, 6, true);
    }
    const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, ratingNames[i], EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + (boxWidth - labelWidth) / 2, ratingTop + labelInset, ratingNames[i], !selected,
                      EpdFontFamily::BOLD);

    char interval[16];
    formatInterval(interval, sizeof(interval), scheduler.previewIntervalSeconds(currentIndex, static_cast<Rating>(i)));
    const int intervalWidth = renderer.getTextWidth(SMALL_FONT_ID, interval);
    renderer.drawText(SMALL_FONT_ID, x + (boxWidth - intervalWidth) / 2, ratingTop + RATING_BOX_HEIGHT + 4, interval);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_FC_EXIT), tr(STR_FC_RATE), tr(STR_FC_PREV), tr(STR_FC_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardsActivity::renderDone() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FLASHCARDS));

  int y = pageHeight / 2 - 40;
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID, y, tr(STR_FC_DONE), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(NOTOSANS_18_FONT_ID) + 12;

  char buf[64];
  snprintf(buf, sizeof(buf), "%s: %u", tr(STR_FC_REVIEWED_TODAY), scheduler.reviewedToday());
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 4;

  const uint32_t wait = scheduler.secondsUntilNextDue();
  if (wait != UINT32_MAX) {
    char interval[16];
    formatInterval(interval, sizeof(interval), wait);
    snprintf(buf, sizeof(buf), "%s %s", tr(STR_FC_NEXT_DUE_IN), interval);
    renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardsActivity::formatInterval(char* buf, size_t bufSize, uint32_t seconds) {
  constexpr uint32_t MINUTE = 60;
  constexpr uint32_t HOUR = 3600;
  constexpr uint32_t DAY = 86400;
  if (seconds < MINUTE) {
    snprintf(buf, bufSize, "<1m");
  } else if (seconds < HOUR) {
    snprintf(buf, bufSize, "%um", static_cast<unsigned>(seconds / MINUTE));
  } else if (seconds < DAY) {
    snprintf(buf, bufSize, "%uh", static_cast<unsigned>(seconds / HOUR));
  } else if (seconds < DAY * 30) {
    snprintf(buf, bufSize, "%ud", static_cast<unsigned>(seconds / DAY));
  } else if (seconds < DAY * 365) {
    snprintf(buf, bufSize, "%umo", static_cast<unsigned>(seconds / (DAY * 30)));
  } else {
    snprintf(buf, bufSize, "%uy", static_cast<unsigned>(seconds / (DAY * 365)));
  }
}
