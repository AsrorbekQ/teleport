#pragma once

#include <cstdint>
#include <string>

#include "HabitStore.h"
#include "activities/UiListActivity.h"

class HabitsActivity final : public UiListActivity {
 public:
  explicit HabitsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("Habits", renderer, mappedInput) {}

  void onEnter() override;
  void render(RenderLock&&) override;

  static constexpr const char* STORE_PATH = "/apps/habits/habits.bin";

 private:
  enum class Screen : uint8_t { NoClock, List, Details, ConfirmDelete };

  static constexpr uint8_t GRID_WEEKS = 12;

  // --- UiListActivity contract ----------------------------------------------
  int listCount() const override { return store.count(); }
  void buildScreen(UiScreen& uiScreen) override;
  void activateIndex(int index) override;
  // The three sub-screens are not lists, so they consume the whole input pass
  // before the base's list navigation and touch routing ever run.
  bool handleCustomInput() override;
  bool handleButtons() override;
  void drawChrome() override;
  void drawFooter() override;

  Screen screen = Screen::List;
  HabitStore store;

  int selectedIndex() const { return nav.selected.load(); }

  void syncClock();
  void toggleSelected();
  void addHabit();
  void deleteSelected();

  void renderNoClock();
  void renderEmptyList();
  void renderDetails();
  void renderConfirmDelete();
  void drawHeaderWithDate();
  void drawGrid(int x, int y, int width, uint32_t today);

  // Row storage for the FreeInkUI list. MAX_HABITS is a compile-time bound, so
  // fixed-capacity storage needs no array growth; the label and streak strings
  // are refreshed in place each build because both track the live check-in
  // state for today.
  std::string rowLabels[HabitStore::MAX_HABITS];
  std::string rowValues[HabitStore::MAX_HABITS];
  freeink::ui::ListItem rowItems[HabitStore::MAX_HABITS]{};
};
