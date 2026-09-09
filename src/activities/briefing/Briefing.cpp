#include "Briefing.h"

#include <ArduinoJson.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "activities/flashcards/FlashcardDeck.h"
#include "activities/flashcards/FlashcardScheduler.h"
#include "activities/flashcards/FlashcardsActivity.h"
#include "activities/habits/HabitStore.h"
#include "activities/habits/HabitsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/DateUtils.h"

namespace Briefing {
namespace {
constexpr const char* TAG = "BRIEF";
constexpr size_t MAX_TASK_BODY = 24 * 1024;  // enough for ~40 tasks; the rest is cut off
constexpr size_t MAX_TASK_TEXT = 90;
constexpr const char* DEFAULT_TODOIST_URL = "https://api.todoist.com/api/v1/tasks/filter?query=today%20%7C%20overdue";
constexpr int SIDE_PADDING = 24;

void trim(std::string& s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) start++;
  s.erase(0, start);
}

// Reads "key=value" lines; calls onPair for each.
template <typename F>
void readKeyValues(const char* path, F onPair) {
  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) return;
  std::string line;
  auto flush = [&]() {
    trim(line);
    if (!line.empty() && line[0] != '#') {
      const size_t eq = line.find('=');
      if (eq != std::string::npos) {
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        trim(key);
        trim(value);
        onPair(key, value);
      }
    }
    line.clear();
  };
  while (file.available() > 0) {
    const char c = static_cast<char>(file.read());
    if (c == '\n') {
      flush();
    } else {
      line.push_back(c);
    }
  }
  flush();
}

const char* weatherText(int code) {
  if (code == 0) return tr(STR_WX_CLEAR);
  if (code <= 2) return tr(STR_WX_PARTLY_CLOUDY);
  if (code == 3) return tr(STR_WX_OVERCAST);
  if (code == 45 || code == 48) return tr(STR_WX_FOG);
  if (code >= 51 && code <= 57) return tr(STR_WX_DRIZZLE);
  if (code >= 61 && code <= 67) return tr(STR_WX_RAIN);
  if (code >= 71 && code <= 77) return tr(STR_WX_SNOW);
  if (code >= 80 && code <= 82) return tr(STR_WX_SHOWERS);
  if (code >= 85 && code <= 86) return tr(STR_WX_SNOW);
  if (code >= 95) return tr(STR_WX_THUNDERSTORM);
  return tr(STR_WX_CLOUDY);
}

bool fetchWeather(const Config& config, Data& data) {
  char url[200];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current_weather=true"
           "&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto",
           static_cast<double>(config.lat), static_cast<double>(config.lon));
  std::string body;
  if (!HttpDownloader::fetchUrl(url, body)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok || doc["current_weather"].isNull()) {
    LOG_ERR(TAG, "Weather JSON invalid");
    return false;
  }
  data.temp = doc["current_weather"]["temperature"] | 0.0f;
  data.wind = doc["current_weather"]["windspeed"] | 0.0f;
  data.weatherCode = doc["current_weather"]["weathercode"] | 0;
  data.tempMax = doc["daily"]["temperature_2m_max"][0] | data.temp;
  data.tempMin = doc["daily"]["temperature_2m_min"][0] | data.temp;
  data.hasWeather = true;
  return true;
}

bool fetchTasks(const Config& config, Data& data) {
  const std::string& url = config.todoistUrl.empty() ? DEFAULT_TODOIST_URL : config.todoistUrl;
  std::string body;
  body.reserve(4096);
  const bool ok = HttpDownloader::fetchUrlBearer(url, config.todoistToken, [&body](const uint8_t* chunk, size_t len) {
    if (body.size() >= MAX_TASK_BODY) return false;
    body.append(reinterpret_cast<const char*>(chunk), std::min(len, MAX_TASK_BODY - body.size()));
    return true;
  });
  if (!ok && body.empty()) return false;

  // Accept both the unified API shape {"results":[...]} and the older REST v2 bare array.
  JsonDocument filter;
  filter[0]["content"] = true;
  filter[0]["due"]["date"] = true;
  filter["results"][0]["content"] = true;
  filter["results"][0]["due"]["date"] = true;
  JsonDocument doc;
  const auto err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err != DeserializationError::Ok && err != DeserializationError::IncompleteInput) {
    LOG_ERR(TAG, "Tasks JSON invalid: %s", err.c_str());
    return false;
  }
  JsonArray tasks = doc.is<JsonArray>() ? doc.as<JsonArray>() : doc["results"].as<JsonArray>();

  char today[12];
  DateUtils::formatDay(today, sizeof(today), DateUtils::todayIndex());
  data.tasks.clear();
  data.tasks.reserve(MAX_TASKS);
  for (JsonObject task : tasks) {
    if (data.tasks.size() >= MAX_TASKS) break;
    const char* content = task["content"] | "";
    if (!*content) continue;
    const char* due = task["due"]["date"] | "";
    std::string text = (*due && strncmp(due, today, 10) < 0) ? "! " : "";
    text += content;
    if (text.size() > MAX_TASK_TEXT) text.resize(MAX_TASK_TEXT);
    data.tasks.push_back(std::move(text));
  }
  data.hasTasks = true;
  return true;
}

int drawRow(GfxRenderer& renderer, int fontId, int y, int width, const char* left, const char* right,
            EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int rightWidth = right ? renderer.getTextWidth(fontId, right, style) : 0;
  const std::string leftText = renderer.truncatedText(fontId, left, width - rightWidth - 12, style);
  renderer.drawText(fontId, SIDE_PADDING, y, leftText.c_str(), true, style);
  if (right) renderer.drawText(fontId, SIDE_PADDING + width - rightWidth, y, right, true, style);
  return y + renderer.getLineHeight(fontId) + 4;
}

int drawSectionTitle(GfxRenderer& renderer, int y, int width, const char* title) {
  y += 10;
  renderer.drawText(UI_12_FONT_ID, SIDE_PADDING, y, title, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 2;
  renderer.fillRect(SIDE_PADDING, y, width, 1, true);
  return y + 8;
}
}  // namespace

bool configExists() { return Storage.exists(CONFIG_PATH); }

Config loadConfig() {
  Config config;
  readKeyValues(CONFIG_PATH, [&config](const std::string& key, const std::string& value) {
    if (key == "enabled")
      config.enabled = value == "1" || value == "true";
    else if (key == "todoist_token")
      config.todoistToken = value;
    else if (key == "todoist_url")
      config.todoistUrl = value;
    else if (key == "city")
      config.city = value;
    else if (key == "lat")
      config.lat = strtof(value.c_str(), nullptr);
    else if (key == "lon")
      config.lon = strtof(value.c_str(), nullptr);
  });
  config.hasLocation = config.lat != 0.0f || config.lon != 0.0f;
  return config;
}

bool loadCache(Data& data) {
  data = Data{};
  data.tasks.reserve(MAX_TASKS);
  readKeyValues(CACHE_PATH, [&data](const std::string& key, const std::string& value) {
    if (key == "fetched") {
      data.fetchedAt = static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10));
    } else if (key == "weather") {
      float temp = 0, tmin = 0, tmax = 0, wind = 0;
      int code = 0;
      if (sscanf(value.c_str(), "%f,%f,%f,%f,%d", &temp, &tmin, &tmax, &wind, &code) == 5) {
        data.temp = temp;
        data.tempMin = tmin;
        data.tempMax = tmax;
        data.wind = wind;
        data.weatherCode = code;
        data.hasWeather = true;
      }
    } else if (key == "tasks") {
      data.hasTasks = true;
    } else if (key == "task" && data.tasks.size() < MAX_TASKS) {
      data.tasks.push_back(value);
    }
  });
  return data.fetchedAt != 0;
}

bool saveCache(const Data& data) {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists(DIR);
  HalFile file;
  if (!Storage.openFileForWrite(TAG, CACHE_PATH, file)) {
    LOG_ERR(TAG, "Cannot write cache");
    return false;
  }
  char line[160];
  int n = snprintf(line, sizeof(line), "fetched=%lu\n", static_cast<unsigned long>(data.fetchedAt));
  file.write(line, n);
  if (data.hasWeather) {
    n = snprintf(line, sizeof(line), "weather=%.1f,%.1f,%.1f,%.1f,%d\n", static_cast<double>(data.temp),
                 static_cast<double>(data.tempMin), static_cast<double>(data.tempMax), static_cast<double>(data.wind),
                 data.weatherCode);
    file.write(line, n);
  }
  if (data.hasTasks) {
    file.write("tasks=1\n", 8);
    for (const auto& task : data.tasks) {
      file.write("task=", 5);
      file.write(task.data(), task.size());
      file.write("\n", 1);
    }
  }
  return true;
}

bool refresh(const Config& config, Data& data, std::string& error) {
  bool any = false;
  error.clear();
  if (config.hasLocation) {
    if (fetchWeather(config, data)) {
      any = true;
    } else {
      error = tr(STR_BF_WEATHER_UNAVAILABLE);
    }
  }
  if (!config.todoistToken.empty()) {
    if (fetchTasks(config, data)) {
      any = true;
    } else {
      error = tr(STR_BF_TASKS_UNAVAILABLE);
    }
  }
  if (any || (!config.hasLocation && config.todoistToken.empty())) {
    data.fetchedAt = DateUtils::nowUtc();
    any = true;
  }
  return any;
}

bool shouldRefreshAtSleep(const Config& config, const Data& data) {
  if (!config.enabled) return false;
  if (!config.hasLocation && config.todoistToken.empty()) return false;
  if (powerManager.getBatteryPercentage() < MIN_BATTERY_FOR_FETCH) return false;
  if (!DateUtils::hasValidTime()) return true;
  const uint32_t now = DateUtils::nowUtc();
  return data.fetchedAt == 0 || now < data.fetchedAt || now - data.fetchedAt >= SLEEP_REFRESH_INTERVAL_S;
}

void render(GfxRenderer& renderer, const Config& config, const Data& data, int bottomInset) {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight() - bottomInset;
  const int width = pageWidth - SIDE_PADDING * 2;
  const int footerHeight = renderer.getLineHeight(SMALL_FONT_ID) + 16;
  const int bottomLimit = pageHeight - footerHeight;
  int y = 28;
  char buf[160];

  // Date
  if (DateUtils::hasValidTime()) {
    const time_t local = static_cast<time_t>(DateUtils::todayIndex()) * DateUtils::SECONDS_PER_DAY;
    struct tm tm;
    gmtime_r(&local, &tm);
    strftime(buf, sizeof(buf), "%A", &tm);
    renderer.drawText(NOTOSANS_18_FONT_ID, SIDE_PADDING, y, buf, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(NOTOSANS_18_FONT_ID);
    strftime(buf, sizeof(buf), "%B %d, %Y", &tm);
    renderer.drawText(UI_12_FONT_ID, SIDE_PADDING, y, buf);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 6;
  } else {
    renderer.drawText(NOTOSANS_18_FONT_ID, SIDE_PADDING, y, tr(STR_APP_CLOCK_NOT_SET), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(NOTOSANS_18_FONT_ID) + 6;
  }

  // Weather
  if (data.hasWeather) {
    snprintf(buf, sizeof(buf),
             "%.0f\xC2\xB0"
             "C  %s",
             static_cast<double>(data.temp), weatherText(data.weatherCode));
    renderer.drawText(NOTOSANS_16_FONT_ID, SIDE_PADDING, y, buf);
    y += renderer.getLineHeight(NOTOSANS_16_FONT_ID);
    snprintf(buf, sizeof(buf), "%s%s%.0f\xC2\xB0 / %.0f\xC2\xB0  %s %.0f km/h", config.city.c_str(),
             config.city.empty() ? "" : "  ", static_cast<double>(data.tempMax), static_cast<double>(data.tempMin),
             tr(STR_BF_WIND), static_cast<double>(data.wind));
    renderer.drawText(UI_10_FONT_ID, SIDE_PADDING, y, buf);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 4;
  } else if (config.hasLocation) {
    renderer.drawText(UI_10_FONT_ID, SIDE_PADDING, y, tr(STR_BF_WEATHER_UNAVAILABLE));
    y += renderer.getLineHeight(UI_10_FONT_ID) + 4;
  }

  // Tasks
  if (!config.todoistToken.empty()) {
    y = drawSectionTitle(renderer, y, width, tr(STR_BF_TODAY));
    if (!data.hasTasks) {
      y = drawRow(renderer, UI_10_FONT_ID, y, width, tr(STR_BF_TASKS_UNAVAILABLE), nullptr);
    } else if (data.tasks.empty()) {
      y = drawRow(renderer, UI_10_FONT_ID, y, width, tr(STR_BF_NO_TASKS), nullptr);
    } else {
      const int reserve = renderer.getLineHeight(UI_12_FONT_ID) * 5;  // keep room for the sections below
      for (const auto& task : data.tasks) {
        if (y + renderer.getLineHeight(NOTOSANS_14_FONT_ID) > bottomLimit - reserve) break;
        const bool overdue = task.rfind("! ", 0) == 0;
        snprintf(buf, sizeof(buf), "%s %s", overdue ? "!" : "\xE2\x80\xA2", overdue ? task.c_str() + 2 : task.c_str());
        y = drawRow(renderer, NOTOSANS_14_FONT_ID, y, width, buf, nullptr,
                    overdue ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      }
    }
  }

  // Habits
  HabitStore habits;
  habits.load(HabitsActivity::STORE_PATH);
  if (habits.count() > 0 && y + renderer.getLineHeight(UI_12_FONT_ID) * 3 < bottomLimit) {
    y = drawSectionTitle(renderer, y, width, tr(STR_HABITS));
    const uint32_t today = DateUtils::todayIndex();
    for (uint8_t i = 0; i < habits.count(); i++) {
      if (y + renderer.getLineHeight(UI_12_FONT_ID) > bottomLimit - renderer.getLineHeight(UI_12_FONT_ID) * 2) break;
      const uint16_t streak = habits.currentStreak(i, today);
      snprintf(buf, sizeof(buf), "%u %s", streak, tr(STR_HB_DAYS));
      std::string label = std::string(habits.isDone(i, today) ? "[x] " : "[  ] ") + habits.name(i);
      y = drawRow(renderer, UI_12_FONT_ID, y, width, label.c_str(), streak > 0 ? buf : "");
    }
  }

  // Flashcards
  {
    FlashcardDeck deck;
    if (deck.open(FlashcardsActivity::DECK_PATH) && y + renderer.getLineHeight(UI_12_FONT_ID) * 3 < bottomLimit) {
      FlashcardScheduler scheduler;
      if (scheduler.init(deck.count(), FlashcardsActivity::PROGRESS_PATH)) {
        const auto counts = scheduler.counts();
        y = drawSectionTitle(renderer, y, width, tr(STR_FLASHCARDS));
        snprintf(buf, sizeof(buf), "%s %u  (%s %u, %s %u)", tr(STR_FC_DUE),
                 counts.newAvailable + counts.learningDue + counts.reviewDue, tr(STR_FC_NEW), counts.newAvailable,
                 tr(STR_FC_REVIEW), counts.reviewDue);
        y = drawRow(renderer, UI_12_FONT_ID, y, width, buf, nullptr);
        scheduler.release();
      }
      deck.close();
    }
  }

  // Footer
  char updated[24] = "";
  if (data.fetchedAt != 0) {
    const time_t local = static_cast<time_t>(data.fetchedAt) + DateUtils::utcOffsetSeconds();
    struct tm tm;
    gmtime_r(&local, &tm);
    strftime(updated, sizeof(updated), "%H:%M", &tm);
  }
  snprintf(buf, sizeof(buf), "%s %s   %s %u%%", tr(STR_BF_UPDATED), data.fetchedAt ? updated : tr(STR_BF_NEVER),
           tr(STR_BF_BATTERY), powerManager.getBatteryPercentage());
  renderer.drawText(SMALL_FONT_ID, SIDE_PADDING, pageHeight - footerHeight + 6, buf);
}

}  // namespace Briefing
