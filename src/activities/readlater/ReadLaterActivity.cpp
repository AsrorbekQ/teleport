#include "ReadLaterActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* TAG = "READLATER";
constexpr int SIDE_PADDING = 20;
}  // namespace

void ReadLaterActivity::onEnter() {
  Activity::onEnter();
  store.load();
  selectedIndex = std::min(selectedIndex, std::max(0, static_cast<int>(store.count()) - 1));
  requestUpdate();
}

void ReadLaterActivity::onExit() {
  Activity::onExit();
  if (wifiWasUsed) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    silentRestart();  // reclaim heap fragmented by the Wi-Fi session, as the RSS app does
  }
}

void ReadLaterActivity::showPopup(const char* message) {
  RenderLock lock;
  GUI.drawPopup(renderer, message);
  renderer.displayBuffer();
}

void ReadLaterActivity::openSelected() {
  if (store.count() == 0) return;
  const auto& entry = store.at(static_cast<size_t>(selectedIndex));
  if (!entry.cached) {
    fetchSelected();
    return;
  }
  activityManager.pushReader(ReadLaterStore::cachePath(entry.url));
}

void ReadLaterActivity::fetchSelected() {
  if (store.count() == 0) return;
  ensureWifiConnected([this]() {
    wifiWasUsed = true;
    showPopup(tr(STR_RL_FETCHING));
    std::string error;
    const bool ok = store.fetch(static_cast<size_t>(selectedIndex), error);
    store.save();
    if (ok) {
      activityManager.pushReader(ReadLaterStore::cachePath(store.at(static_cast<size_t>(selectedIndex)).url));
    } else {
      LOG_ERR(TAG, "Fetch failed: %s", error.c_str());
      statusMessage = std::string(tr(STR_RL_FETCH_FAILED)) + ": " + error;
      requestUpdate();
    }
  });
}

void ReadLaterActivity::fetchAll() {
  if (store.count() == store.cachedCount()) return;
  ensureWifiConnected([this]() {
    wifiWasUsed = true;
    size_t fetched = 0;
    size_t failed = 0;
    char progress[48];
    for (size_t i = 0; i < store.count(); i++) {
      if (store.at(i).cached) continue;
      snprintf(progress, sizeof(progress), "%s %u/%u", tr(STR_RL_FETCHING), static_cast<unsigned>(fetched + failed + 1),
               static_cast<unsigned>(store.count() - store.cachedCount()));
      showPopup(progress);
      std::string error;
      if (store.fetch(i, error)) {
        fetched++;
      } else {
        failed++;
        LOG_ERR(TAG, "Fetch failed for %s: %s", store.at(i).url.c_str(), error.c_str());
      }
    }
    store.save();
    snprintf(progress, sizeof(progress), "%s: %u, %s: %u", tr(STR_RL_CACHED), static_cast<unsigned>(fetched),
             tr(STR_RL_FETCH_FAILED), static_cast<unsigned>(failed));
    statusMessage = progress;
    requestUpdate();
  });
}

void ReadLaterActivity::loop() {
  using Button = MappedInputManager::Button;
  const bool back = mappedInput.wasReleased(Button::Back);
  const bool confirm = mappedInput.wasReleased(Button::Confirm);
  const bool left = mappedInput.wasReleased(Button::Left);
  const bool right = mappedInput.wasReleased(Button::Right);

  if (screen == Screen::ConfirmDelete) {
    if (back) {
      screen = Screen::List;
      requestUpdate();
    } else if (confirm) {
      store.remove(static_cast<size_t>(selectedIndex));
      store.save();
      selectedIndex = std::min(selectedIndex, std::max(0, static_cast<int>(store.count()) - 1));
      screen = Screen::List;
      requestUpdate();
    }
    return;
  }

  if (back) {
    finish();
    return;
  }
  statusMessage.clear();
  if (confirm) {
    openSelected();
  } else if (right) {
    fetchAll();
  } else if (left && store.count() > 0) {
    screen = Screen::ConfirmDelete;
    requestUpdate();
  } else if (store.count() > 0) {
    const int itemCount = static_cast<int>(store.count());
    buttonNavigator.onPressAndContinuous({Button::Down}, [this, itemCount] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
      requestUpdate();
    });
    buttonNavigator.onPressAndContinuous({Button::Up}, [this, itemCount] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
      requestUpdate();
    });
  }
}

void ReadLaterActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (screen == Screen::ConfirmDelete) {
    renderConfirmDelete();
  } else {
    renderList();
  }
  renderer.displayBuffer();
}

void ReadLaterActivity::renderList() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  char subtitle[32];
  snprintf(subtitle, sizeof(subtitle), "%u / %u %s", static_cast<unsigned>(store.cachedCount()),
           static_cast<unsigned>(store.count()), tr(STR_RL_SAVED));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READ_LATER),
                 store.count() > 0 ? subtitle : nullptr);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int statusHeight = statusMessage.empty() ? 0 : renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2 - statusHeight;

  if (store.count() == 0) {
    const int y = contentTop + contentHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_RL_EMPTY), true, EpdFontFamily::BOLD);
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_RL_EMPTY_HINT), pageWidth - SIDE_PADDING * 2, 3);
    int hintY = y + renderer.getLineHeight(UI_12_FONT_ID) + 8;
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_10_FONT_ID, hintY, line.c_str());
      hintY += renderer.getLineHeight(UI_10_FONT_ID);
    }
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(store.count()), selectedIndex,
        [this](int index) { return store.displayName(static_cast<size_t>(index)); },
        [this](int index) {
          return std::string(store.at(static_cast<size_t>(index)).cached ? tr(STR_RL_CACHED) : tr(STR_RL_NOT_FETCHED));
        });
  }

  if (!statusMessage.empty()) {
    const std::string line = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - SIDE_PADDING * 2);
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight + metrics.verticalSpacing, line.c_str());
  }

  const bool selectedCached = store.count() > 0 && store.at(static_cast<size_t>(selectedIndex)).cached;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), selectedCached ? tr(STR_RL_OPEN) : tr(STR_RL_FETCH),
                                            tr(STR_DELETE), tr(STR_RL_FETCH_ALL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ReadLaterActivity::renderConfirmDelete() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READ_LATER));

  const int titleY = pageHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 6;
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, tr(STR_RL_DELETE_CONFIRM), true, EpdFontFamily::BOLD);
  const std::string name = renderer.truncatedText(
      UI_12_FONT_ID, store.displayName(static_cast<size_t>(selectedIndex)).c_str(), pageWidth - SIDE_PADDING * 2);
  int y = titleY + renderer.getLineHeight(UI_12_FONT_ID) + 8;
  renderer.drawCenteredText(UI_12_FONT_ID, y, name.c_str());
  y += renderer.getLineHeight(UI_12_FONT_ID) + 12;
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_RL_DELETE_HINT));

  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DELETE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
