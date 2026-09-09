#pragma once

#include <cstdint>

#include "HabitStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class HabitsActivity final : public Activity {
 public:
  explicit HabitsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Habits", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  static constexpr const char* STORE_PATH = "/apps/habits/habits.bin";

 private:
  enum class Screen : uint8_t { NoClock, List, Details, ConfirmDelete };

  static constexpr uint8_t GRID_WEEKS = 12;

  Screen screen = Screen::List;
  HabitStore store;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

  void syncClock();
  void toggleSelected();
  void addHabit();
  void deleteSelected();

  void renderNoClock();
  void renderList();
  void renderDetails();
  void renderConfirmDelete();
  void drawHeaderWithDate();
  void drawGrid(int x, int y, int width, uint32_t today);
};
