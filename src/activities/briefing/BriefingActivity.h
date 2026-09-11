#pragma once

#include <string>

#include "Briefing.h"
#include "activities/Activity.h"

// Shows the cached briefing and refreshes it on demand over Wi-Fi.
class BriefingActivity final : public Activity {
 public:
  explicit BriefingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Briefing", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  Briefing::Config config;
  Briefing::Data data;
  std::string statusMessage;
  bool wifiWasUsed = false;
  int scroll = 0;
  Briefing::Page page;

  void refreshNow();
};
