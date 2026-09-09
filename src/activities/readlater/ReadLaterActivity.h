#pragma once

#include <cstdint>
#include <string>

#include "ReadLaterStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ReadLaterActivity final : public Activity {
 public:
  explicit ReadLaterActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadLater", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Screen : uint8_t { List, ConfirmDelete };

  Screen screen = Screen::List;
  ReadLaterStore store;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  bool wifiWasUsed = false;
  std::string statusMessage;

  void openSelected();
  void fetchSelected();
  void fetchAll();
  void showPopup(const char* message);
  void renderList();
  void renderConfirmDelete();
};
