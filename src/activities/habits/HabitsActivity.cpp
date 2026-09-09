#include "HabitsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <variant>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/WifiConnectHelper.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DateUtils.h"

namespace {
constexpr const char* TAG = "HABITS";
constexpr int SIDE_PADDING = 20;
constexpr int GRID_CELL_GAP = 3;
constexpr int GRID_MAX_CELL = 22;

// Day index 0 (1970-01-01) was a Thursday; returns 0 for Monday.
uint8_t weekdayOf(uint32_t day) { return static_cast<uint8_t>((day + 3) % 7); }
}  // namespace

void HabitsActivity::onEnter() {
  Activity::onEnter();
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/habits");
  store.load(STORE_PATH);
  screen = DateUtils::hasValidTime() ? Screen::List : Screen::NoClock;
  requestUpdate();
}

void HabitsActivity::onExit() { Activity::onExit(); }

void HabitsActivity::syncClock() {
  ensureWifiConnected([this]() {
    {
      RenderLock lock;
      GUI.drawPopup(renderer, tr(STR_SYNCING_TIME));
      renderer.displayBuffer();
    }
    if (WifiConnectHelper::waitForTimeSync() && DateUtils::hasValidTime()) {
      screen = Screen::List;
    }
    requestUpdate();
  });
}

void HabitsActivity::toggleSelected() {
  if (store.count() == 0) return;
  const uint32_t today = DateUtils::todayIndex();
  const auto index = static_cast<uint8_t>(selectedIndex);
  store.setDone(index, today, !store.isDone(index, today));
  // Saved immediately: a check-in is rare and losing it to auto-sleep would be worse than the 1 KB write.
  store.save();
  requestUpdate();
}

void HabitsActivity::addHabit() {
  if (store.count() >= HabitStore::MAX_HABITS) {
    LOG_INF(TAG, "Habit limit reached");
    return;
  }
  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_HB_NEW_HABIT), "",
                                                          HabitStore::NAME_LENGTH - 1);
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto* keyboardResult = std::get_if<KeyboardResult>(&result.data);
    if (keyboardResult && !keyboardResult->text.empty() && store.add(keyboardResult->text.c_str())) {
      store.save();
      selectedIndex = store.count() - 1;
    }
  });
}

void HabitsActivity::deleteSelected() {
  store.remove(static_cast<uint8_t>(selectedIndex));
  store.save();
  selectedIndex = std::max(0, std::min(selectedIndex, static_cast<int>(store.count()) - 1));
  screen = Screen::List;
  requestUpdate();
}

void HabitsActivity::loop() {
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

    case Screen::List:
      if (back) {
        finish();
        return;
      }
      if (confirm) {
        toggleSelected();
      } else if (right) {
        addHabit();
      } else if (left && store.count() > 0) {
        screen = Screen::Details;
        requestUpdate();
      } else if (store.count() > 0) {
        const int itemCount = store.count();
        buttonNavigator.onPressAndContinuous({Button::Down}, [this, itemCount] {
          selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
          requestUpdate();
        });
        buttonNavigator.onPressAndContinuous({Button::Up}, [this, itemCount] {
          selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
          requestUpdate();
        });
      }
      break;

    case Screen::Details:
      if (back) {
        screen = Screen::List;
        requestUpdate();
      } else if (confirm) {
        toggleSelected();
      } else if (left) {
        screen = Screen::ConfirmDelete;
        requestUpdate();
      }
      break;

    case Screen::ConfirmDelete:
      if (back) {
        screen = Screen::Details;
        requestUpdate();
      } else if (confirm) {
        deleteSelected();
      }
      break;
  }
}

void HabitsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (screen) {
    case Screen::NoClock:
      renderNoClock();
      break;
    case Screen::List:
      renderList();
      break;
    case Screen::Details:
      renderDetails();
      break;
    case Screen::ConfirmDelete:
      renderConfirmDelete();
      break;
  }
  renderer.displayBuffer();
}

void HabitsActivity::drawHeaderWithDate() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  char date[16];
  DateUtils::formatDay(date, sizeof(date), DateUtils::todayIndex());
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_HABITS),
                 date);
}

void HabitsActivity::renderNoClock() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HABITS));

  const int titleY = pageHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 6;
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, tr(STR_APP_CLOCK_NOT_SET), true, EpdFontFamily::BOLD);
  int y = titleY + renderer.getLineHeight(UI_12_FONT_ID) + 12;
  const auto lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_APP_CLOCK_HINT), pageWidth - SIDE_PADDING * 2, 3);
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CLOCK_SYNC), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void HabitsActivity::renderList() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  drawHeaderWithDate();

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const uint32_t today = DateUtils::todayIndex();

  if (store.count() == 0) {
    const int y = contentTop + contentHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_HB_EMPTY), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID) + 8, tr(STR_HB_EMPTY_HINT));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, store.count(), selectedIndex,
        [this, today](int index) {
          const auto habit = static_cast<uint8_t>(index);
          return std::string(store.isDone(habit, today) ? "[x] " : "[  ] ") + store.name(habit);
        },
        nullptr, nullptr,
        [this, today](int index) {
          const uint16_t streak = store.currentStreak(static_cast<uint8_t>(index), today);
          if (streak == 0) return std::string();
          char buf[24];
          snprintf(buf, sizeof(buf), "%u %s", streak, tr(STR_HB_DAYS));
          return std::string(buf);
        });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_HB_TOGGLE), tr(STR_HB_DETAILS), tr(STR_HB_ADD));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void HabitsActivity::renderDetails() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const uint32_t today = DateUtils::todayIndex();
  const auto habit = static_cast<uint8_t>(selectedIndex);
  const bool doneToday = store.isDone(habit, today);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, store.name(habit),
                 doneToday ? tr(STR_HB_DONE_TODAY) : tr(STR_HB_NOT_DONE_TODAY));

  struct Row {
    const char* label;
    unsigned value;
    unsigned total;
  };
  const Row rows[] = {
      {tr(STR_HB_STREAK), store.currentStreak(habit, today), 0},
      {tr(STR_HB_BEST_STREAK), store.bestStreak(habit, today), 0},
      {tr(STR_HB_LAST_30_DAYS), store.doneInLast(habit, today, 30), 30},
      {tr(STR_HB_TOTAL), store.total(habit), 0},
  };
  const int rowHeight = metrics.listRowHeight;
  const int textInset = (rowHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  char value[24];
  for (const auto& row : rows) {
    renderer.drawText(UI_12_FONT_ID, SIDE_PADDING, y + textInset, row.label);
    if (row.total > 0) {
      snprintf(value, sizeof(value), "%u / %u", row.value, row.total);
    } else {
      snprintf(value, sizeof(value), "%u", row.value);
    }
    const int valueWidth = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, pageWidth - SIDE_PADDING - valueWidth, y + textInset, value, true,
                      EpdFontFamily::BOLD);
    y += rowHeight;
  }

  y += metrics.verticalSpacing;
  const int gridBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (y < gridBottom) drawGrid(SIDE_PADDING, y, pageWidth - SIDE_PADDING * 2, today);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_HB_TOGGLE), tr(STR_DELETE), "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// GitHub-style grid: one column per week, Monday at the top, current week in the last column.
void HabitsActivity::drawGrid(int x, int y, int width, uint32_t today) {
  const int columns = GRID_WEEKS + 1;
  const int cell = std::min(GRID_MAX_CELL, width / columns - GRID_CELL_GAP);
  if (cell < 4) return;
  const int pitch = cell + GRID_CELL_GAP;
  const int gridWidth = columns * pitch - GRID_CELL_GAP;
  const int originX = x + (width - gridWidth) / 2;
  const uint32_t thisMonday = today - weekdayOf(today);
  const uint32_t firstMonday = thisMonday - static_cast<uint32_t>(GRID_WEEKS) * 7;
  const auto habit = static_cast<uint8_t>(selectedIndex);

  for (int col = 0; col < columns; col++) {
    for (int row = 0; row < 7; row++) {
      const uint32_t day = firstMonday + static_cast<uint32_t>(col) * 7 + static_cast<uint32_t>(row);
      if (day > today) continue;
      const int cx = originX + col * pitch;
      const int cy = y + row * pitch;
      if (store.isDone(habit, day)) {
        renderer.fillRect(cx, cy, cell, cell, true);
      } else {
        renderer.drawRect(cx, cy, cell, cell, true);
      }
    }
  }
}

void HabitsActivity::renderConfirmDelete() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HABITS));

  const int titleY = pageHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 6;
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, tr(STR_HB_DELETE_CONFIRM), true, EpdFontFamily::BOLD);
  const std::string name = renderer.truncatedText(UI_12_FONT_ID, store.name(static_cast<uint8_t>(selectedIndex)),
                                                  pageWidth - SIDE_PADDING * 2);
  int y = titleY + renderer.getLineHeight(UI_12_FONT_ID) + 8;
  renderer.drawCenteredText(UI_12_FONT_ID, y, name.c_str());
  y += renderer.getLineHeight(UI_12_FONT_ID) + 12;
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_HB_DELETE_HINT));

  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DELETE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
