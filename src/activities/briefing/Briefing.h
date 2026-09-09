#pragma once

#include <GfxRenderer.h>

#include <cstdint>
#include <string>
#include <vector>

// Morning briefing: weather (Open-Meteo), today's Todoist tasks, habit streaks and
// flashcards due. Data is fetched over Wi-Fi at sleep time (when enabled) or on
// demand, cached on the SD card, and rendered as a static full-screen page.
namespace Briefing {

constexpr const char* DIR = "/apps/briefing";
constexpr const char* CONFIG_PATH = "/apps/briefing/config.txt";
constexpr const char* CACHE_PATH = "/apps/briefing/cache.txt";
constexpr uint32_t SLEEP_REFRESH_INTERVAL_S = 6 * 3600;
constexpr uint16_t MIN_BATTERY_FOR_FETCH = 20;
constexpr size_t MAX_TASKS = 12;

struct Config {
  bool enabled = false;  // fetch and show at sleep time
  bool hasLocation = false;
  float lat = 0;
  float lon = 0;
  std::string city;
  std::string todoistToken;
  std::string todoistUrl;  // optional override of the tasks endpoint
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

// Draws the briefing into the framebuffer (does not call displayBuffer).
void render(GfxRenderer& renderer, const Config& config, const Data& data, int bottomInset = 0);

}  // namespace Briefing
