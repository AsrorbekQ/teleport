#include "BriefingActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/util/WifiConnectHelper.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DateUtils.h"

void BriefingActivity::onEnter() {
  Activity::onEnter();
  config = Briefing::loadConfig();
  Briefing::loadCache(data);
  requestUpdate();
}

void BriefingActivity::onExit() {
  Activity::onExit();
  if (wifiWasUsed) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    silentRestart();
  }
}

void BriefingActivity::refreshNow() {
  ensureWifiConnected([this]() {
    wifiWasUsed = true;
    {
      RenderLock lock;
      GUI.drawPopup(renderer, tr(STR_BF_UPDATING));
      renderer.displayBuffer();
    }
    if (!DateUtils::hasValidTime()) WifiConnectHelper::waitForTimeSync();
    std::string error;
    if (Briefing::refresh(config, data, error)) {
      Briefing::saveCache(data);
    }
    statusMessage = error;
    requestUpdate();
  });
}

void BriefingActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back)) {
    finish();
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    refreshNow();
  }
}

void BriefingActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  Briefing::render(renderer, config, data, metrics.buttonHintsHeight + renderer.getLineHeight(UI_10_FONT_ID) + 16);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const char* note = nullptr;
  if (!statusMessage.empty()) {
    note = statusMessage.c_str();
  } else if (!Briefing::configExists()) {
    note = tr(STR_BF_NO_CONFIG_HINT);
  } else if (!config.enabled) {
    note = tr(STR_BF_DISABLED_HINT);
  }
  if (note) {
    const int y = pageHeight - metrics.buttonHintsHeight - renderer.getLineHeight(UI_10_FONT_ID) * 2 - 10;
    renderer.fillRect(0, y - 4, pageWidth, renderer.getLineHeight(UI_10_FONT_ID) + 8, false);
    renderer.drawCenteredText(UI_10_FONT_ID, y, renderer.truncatedText(UI_10_FONT_ID, note, pageWidth - 20).c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_BF_REFRESH), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
