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

namespace fui = freeink::ui;

namespace {
constexpr const char* TAG = "READLATER";
constexpr int SIDE_PADDING = 20;
}  // namespace

void ReadLaterActivity::onEnter() {
  UiListActivity::onEnter();
  store.load();
  rebuildRowItems();
}

void ReadLaterActivity::onExit() {
  Activity::onExit();
  if (wifiWasUsed) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    silentRestart();  // reclaim heap fragmented by the Wi-Fi session, as the RSS app does
  }
}

// Titles come from the store (a fetch can fill in a missing one), so the row
// array is rebuilt whenever the queue itself changes rather than per repaint.
void ReadLaterActivity::rebuildRowItems() {
  const size_t count = store.count();
  rowLabels.assign(count, std::string());
  rowItems.assign(count, fui::ListItem{});
  for (size_t i = 0; i < count; i++) {
    rowLabels[i] = store.displayName(i);
    rowItems[i].label = rowLabels[i].c_str();
    rowItems[i].actionValue = static_cast<int16_t>(i);
  }
}

void ReadLaterActivity::showPopup(const char* message) {
  RenderLock lock;
  GUI.drawPopup(renderer, message);
  renderer.displayBuffer();
}

void ReadLaterActivity::openSelected() {
  if (store.count() == 0) return;
  const auto& entry = store.at(static_cast<size_t>(selectedIndex()));
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
    const bool ok = store.fetch(static_cast<size_t>(selectedIndex()), error);
    store.save();
    rebuildRowItems();
    if (ok) {
      activityManager.pushReader(ReadLaterStore::cachePath(store.at(static_cast<size_t>(selectedIndex())).url));
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
    rebuildRowItems();
    snprintf(progress, sizeof(progress), "%s: %u, %s: %u", tr(STR_RL_CACHED), static_cast<unsigned>(fetched),
             tr(STR_RL_FETCH_FAILED), static_cast<unsigned>(failed));
    statusMessage = progress;
    requestUpdate();
  });
}

// The list screen returns false so the base runs its own Back/Confirm handling,
// touch routing and selection navigation; the delete prompt owns the pass.
bool ReadLaterActivity::handleCustomInput() {
  if (screen != Screen::ConfirmDelete) return false;

  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back)) {
    screen = Screen::List;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    const int index = selectedIndex();
    if (index >= 0 && index < static_cast<int>(store.count())) {
      store.remove(static_cast<size_t>(index));
      store.save();
      rebuildRowItems();
    }
    nav.requestSelection(std::min(index, std::max(0, static_cast<int>(store.count()) - 1)));
    screen = Screen::List;
    requestUpdate();
  }
  return true;
}

bool ReadLaterActivity::handleButtons() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back)) {
    finish();
    return true;
  }
  statusMessage.clear();

  if (mappedInput.wasReleased(Button::Confirm)) {
    const int index = selectedIndex();
    if (index >= 0 && index < listCount()) activateIndex(index);
    return true;
  }
  if (mappedInput.wasReleased(Button::Right)) {
    fetchAll();
    return true;
  }
  if (mappedInput.wasReleased(Button::Left) && store.count() > 0) {
    screen = Screen::ConfirmDelete;
    requestUpdate();
    return true;
  }
  return false;
}

void ReadLaterActivity::activateIndex(const int index) {
  nav.selected = index;
  // Opening the reader (or starting a fetch) leaves this screen; a lingering
  // flash would gray an unrelated row on the next render.
  app.clearTapFlash();
  openSelected();
}

void ReadLaterActivity::buildScreen(UiScreen& uiScreen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the status line (when there is
  // one) and the button hints, both of which drawFooter() paints.
  const int statusHeight =
      statusMessage.empty() ? 0 : renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing * 2;
  uiScreen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight + statusHeight), 0});
  uiScreen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // The titles were built by rebuildRowItems(); only the cached flag is live,
  // so its subtitle is refreshed here (pointer reassignment onto tr() strings —
  // no allocation).
  const auto count = static_cast<uint16_t>(std::min(rowItems.size(), store.count()));
  for (uint16_t i = 0; i < count; i++) {
    rowItems[i].subtitle = store.at(i).cached ? tr(STR_RL_CACHED) : tr(STR_RL_NOT_FETCHED);
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = count;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(uiScreen, props);
  uiScreen.list(props);
}

void ReadLaterActivity::render(RenderLock&& lock) {
  // Only the populated list renders through the FreeInkUI app; the empty state
  // and the delete prompt are drawn directly, exactly as they always were.
  if (screen == Screen::List && store.count() > 0) {
    UiListActivity::render(std::move(lock));
    return;
  }

  renderer.clearScreen();
  if (screen == Screen::ConfirmDelete) {
    renderConfirmDelete();
  } else {
    renderEmptyList();
  }
  renderer.displayBuffer();
}

void ReadLaterActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  char subtitle[32];
  snprintf(subtitle, sizeof(subtitle), "%u / %u %s", static_cast<unsigned>(store.cachedCount()),
           static_cast<unsigned>(store.count()), tr(STR_RL_SAVED));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_READ_LATER), store.count() > 0 ? subtitle : nullptr);
}

void ReadLaterActivity::drawFooter() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  if (!statusMessage.empty()) {
    const std::string line = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - SIDE_PADDING * 2);
    const int statusY = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing * 2 -
                        renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawCenteredText(UI_10_FONT_ID, statusY, line.c_str());
  }

  const int index = selectedIndex();
  const bool selectedCached =
      index >= 0 && index < static_cast<int>(store.count()) && store.at(static_cast<size_t>(index)).cached;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), selectedCached ? tr(STR_RL_OPEN) : tr(STR_RL_FETCH),
                                            tr(STR_DELETE), tr(STR_RL_FETCH_ALL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ReadLaterActivity::renderEmptyList() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  drawChrome();

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int y = contentTop + contentHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_RL_EMPTY), true, EpdFontFamily::BOLD);
  const auto lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_RL_EMPTY_HINT), pageWidth - SIDE_PADDING * 2, 3);
  int hintY = y + renderer.getLineHeight(UI_12_FONT_ID) + 8;
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_10_FONT_ID, hintY, line.c_str());
    hintY += renderer.getLineHeight(UI_10_FONT_ID);
  }

  drawFooter();
}

void ReadLaterActivity::renderConfirmDelete() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READ_LATER));

  const int titleY = pageHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 6;
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, tr(STR_RL_DELETE_CONFIRM), true, EpdFontFamily::BOLD);
  const std::string name = renderer.truncatedText(
      UI_12_FONT_ID, store.displayName(static_cast<size_t>(selectedIndex())).c_str(), pageWidth - SIDE_PADDING * 2);
  int y = titleY + renderer.getLineHeight(UI_12_FONT_ID) + 8;
  renderer.drawCenteredText(UI_12_FONT_ID, y, name.c_str());
  y += renderer.getLineHeight(UI_12_FONT_ID) + 12;
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_RL_DELETE_HINT));

  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DELETE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
