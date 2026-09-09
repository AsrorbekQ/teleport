#include "AppRegistry.h"

#include "activities/briefing/BriefingActivity.h"
#include "activities/dice/DiceActivity.h"
#include "activities/flashcards/FlashcardsActivity.h"
#include "activities/habits/HabitsActivity.h"
#include "activities/readlater/ReadLaterActivity.h"
#include "activities/rss/RssActivity.h"

// System Activities
#include "I18n.h"
#include "OpdsServerStore.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/home/RecentBooksActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/settings/OpdsServerListActivity.h"
#include "activities/settings/SettingsActivity.h"

AppRegistry& AppRegistry::getInstance() {
  static AppRegistry instance;
  return instance;
}

AppRegistry::AppRegistry() {
  // Browse Files
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_BROWSE_FILES); }, UIIcon::Folder,
      [](GfxRenderer& r, MappedInputManager& i) { return std::make_unique<FileBrowserActivity>(r, i); }));

  // Recent Books
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_MENU_RECENT_BOOKS); }, UIIcon::Recent,
      [](GfxRenderer& r, MappedInputManager& i) { return std::make_unique<RecentBooksActivity>(r, i); }));

  // OPDS Browser (conditionally visible)
  apps.push_back(std::make_unique<App>([]() { return tr(STR_OPDS_BROWSER); }, UIIcon::Library,
                                       [](GfxRenderer& r, MappedInputManager& i) -> std::unique_ptr<Activity> {
                                         const auto& servers = OPDS_STORE.getServers();
                                         if (servers.size() == 1) {
                                           return std::make_unique<OpdsBookBrowserActivity>(r, i, servers[0]);
                                         } else {
                                           return std::make_unique<OpdsServerListActivity>(r, i, true);
                                         }
                                       },
                                       []() { return OPDS_STORE.hasServers(); }));

  // File Transfer
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_FILE_TRANSFER); }, UIIcon::Transfer,
      [](GfxRenderer& r, MappedInputManager& i) { return std::make_unique<CrossPointWebServerActivity>(r, i); }));

  // Settings
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_SETTINGS_TITLE); }, UIIcon::Settings,
      [](GfxRenderer& r, MappedInputManager& i) { return std::make_unique<SettingsActivity>(r, i); }));

  // Flashcards App
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_FLASHCARDS); }, UIIcon::Flashcards,
      [](GfxRenderer& r, MappedInputManager& i) { return std::make_unique<FlashcardsActivity>(r, i); }));

  // Habits App
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_HABITS); }, UIIcon::Habits,
      [](GfxRenderer& r, MappedInputManager& i) { return std::make_unique<HabitsActivity>(r, i); }));

  // Read Later App
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_READ_LATER); }, UIIcon::ReadLater,
      [](GfxRenderer& r, MappedInputManager& i) { return std::make_unique<ReadLaterActivity>(r, i); }));

  // Briefing App
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_BRIEFING); }, UIIcon::Dashboard,
      [](GfxRenderer& r, MappedInputManager& i) { return std::make_unique<BriefingActivity>(r, i); }));

  // RSS Feed App
  apps.push_back(std::make_unique<App>("RSS Feed", UIIcon::Rss, [](GfxRenderer& r, MappedInputManager& i) {
    return std::make_unique<RssActivity>(r, i);
  }));

  // Dice App
  apps.push_back(std::make_unique<App>("Dice", UIIcon::Dice, [](GfxRenderer& r, MappedInputManager& i) {
    return std::make_unique<DiceActivity>(r, i);
  }));
}
