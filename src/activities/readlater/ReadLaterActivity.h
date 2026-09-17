#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ReadLaterStore.h"
#include "activities/UiListActivity.h"

class ReadLaterActivity final : public UiListActivity {
 public:
  explicit ReadLaterActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("ReadLater", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  enum class Screen : uint8_t { List, ConfirmDelete };

  // --- UiListActivity contract ----------------------------------------------
  int listCount() const override { return static_cast<int>(store.count()); }
  void buildScreen(UiScreen& uiScreen) override;
  void activateIndex(int index) override;
  // The delete prompt is not a list, so it consumes the whole input pass before
  // the base's list navigation and touch routing ever run.
  bool handleCustomInput() override;
  bool handleButtons() override;
  void drawChrome() override;
  void drawFooter() override;

  Screen screen = Screen::List;
  ReadLaterStore store;
  bool wifiWasUsed = false;
  std::string statusMessage;

  int selectedIndex() const { return nav.selected.load(); }

  void openSelected();
  void fetchSelected();
  void fetchAll();
  void showPopup(const char* message);
  void renderEmptyList();
  void renderConfirmDelete();

  // Row storage for the FreeInkUI list. Titles only change when the queue
  // reloads or a fetch fills one in, so they are rebuilt on those events rather
  // than on every repaint; the cached/not-fetched subtitle is a tr() pointer
  // refreshed in place by buildScreen().
  void rebuildRowItems();
  std::vector<std::string> rowLabels;
  std::vector<freeink::ui::ListItem> rowItems;
};
