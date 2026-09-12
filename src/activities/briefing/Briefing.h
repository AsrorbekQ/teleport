#pragma once

#include <GfxRenderer.h>

#include <cstdint>
#include <string>
#include <vector>

// Morning briefing: weather (Open-Meteo), today's tasks (Apple Reminders and
// Calendar, served as plain text by Nest on the local network), habit streaks and
// flashcards due. Data is fetched over Wi-Fi at sleep time (when enabled) or on
// demand, cached on the SD card, and rendered as a static full-screen page.
namespace Briefing {

constexpr const char* DIR = "/apps/briefing";
constexpr const char* CONFIG_PATH = "/apps/briefing/config.txt";
constexpr const char* CACHE_PATH = "/apps/briefing/cache.txt";
constexpr uint32_t SLEEP_REFRESH_INTERVAL_S = 6 * 3600;
constexpr uint16_t MIN_BATTERY_FOR_FETCH = 20;
constexpr size_t MAX_TASKS = 12;

enum class SleepRefresh : uint8_t { Never = 0, Stale = 1, Always = 2 };

struct Config {
  bool enabled = false;  // show the briefing as the sleep screen
  // When to fetch at sleep time. The device has no battery-backed clock, so "stale" only
  // trusts the cache age after an NTP sync this session; otherwise it fetches only an empty cache.
  SleepRefresh sleepRefresh = SleepRefresh::Stale;
  bool hasLocation = false;
  float lat = 0;
  float lon = 0;
  std::string city;
  std::string tasksUrl;  // plain text, one task per line; "! " prefix marks overdue
};

struct Data {
  uint32_t fetchedAt = 0;  // UTC seconds, 0 = never
  bool hasWeather = false;
  float temp = 0;
  float tempMin = 0;
  float tempMax = 0;
  float wind = 0;
  int weatherCode = 0;
  bool hasTasks = false;
  std::vector<std::string> tasks;
};

bool configExists();
Config loadConfig();
bool loadCache(Data& data);
bool saveCache(const Data& data);

// Fetches weather and tasks. Wi-Fi must already be connected. Returns true if anything succeeded.
bool refresh(const Config& config, Data& data, std::string& error);
bool shouldRefreshAtSleep(const Config& config, const Data& data);

struct Page {
  int nextScroll = -1;  // scroll value that shows the next page, -1 when everything fit
  int viewHeight = 0;   // height of the scrolling body
};

// Draws the briefing into the framebuffer (does not call displayBuffer). Date and
// weather stay at the top; tasks, habits and flashcards scroll by whole lines
// starting at `scroll` (a value previously returned in Page::nextScroll).
Page render(GfxRenderer& renderer, const Config& config, const Data& data, int bottomInset = 0, int scroll = 0);

}  // namespace Briefing
