#include "Briefing.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

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
constexpr size_t MAX_TASK_BODY = 4 * 1024;  // Nest sends at most a screenful of lines
constexpr size_t MAX_TASK_TEXT = 90;
constexpr int MAX_TASK_LINES = 3;
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
  body.reserve(2048);  // reserved before the TLS session so nothing grows while the heap is at its low
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

// Nest answers with one task per line, calendar events first ("09:30 Standup"),
// then reminders; overdue reminders carry a "! " prefix. Blank and "#" lines are skipped.
// "http://name.local:port/..." -> "http://<ip>:port/...": lwIP's resolver does not do mDNS,
// and the Mac's DHCP address changes, so Nest is addressed by its Bonjour name.
std::string resolveLocalHost(const std::string& url) {
  const size_t hostStart = url.find("://");
  if (hostStart == std::string::npos) return url;
  const size_t start = hostStart + 3;
  size_t end = url.find_first_of(":/", start);
  if (end == std::string::npos) end = url.size();
  std::string host = url.substr(start, end - start);
  if (host.size() < 7 || host.compare(host.size() - 6, 6, ".local") != 0) return url;
  host.resize(host.size() - 6);
  if (!MDNS.begin("crosspoint")) {
    LOG_ERR(TAG, "mDNS start failed");
    return url;
  }
  const IPAddress ip = MDNS.queryHost(host.c_str(), 3000);
  MDNS.end();
  if (ip == IPAddress()) {
    LOG_ERR(TAG, "mDNS: %s.local not found", host.c_str());
    return url;
  }
  LOG_INF(TAG, "mDNS: %s.local -> %s", host.c_str(), ip.toString().c_str());
  return url.substr(0, start) + ip.toString().c_str() + url.substr(end);
}

bool fetchTasks(const Config& config, Data& data) {
  std::string body;
  body.reserve(MAX_TASK_BODY);  // reserved before connecting; see fetchWeather
  const bool ok =
      HttpDownloader::fetchUrl(resolveLocalHost(config.tasksUrl), [&body](const uint8_t* chunk, size_t len) {
        if (body.size() >= MAX_TASK_BODY) return false;
        body.append(reinterpret_cast<const char*>(chunk), std::min(len, MAX_TASK_BODY - body.size()));
        return true;
      });
  if (!ok && body.empty()) return false;

  data.tasks.clear();
  data.tasks.reserve(MAX_TASKS);
  size_t pos = 0;
  while (pos < body.size() && data.tasks.size() < MAX_TASKS) {
    size_t eol = body.find('\n', pos);
    if (eol == std::string::npos) eol = body.size();
    std::string line = body.substr(pos, eol - pos);
    pos = eol + 1;
    trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line.size() > MAX_TASK_TEXT) line.resize(MAX_TASK_TEXT);
    data.tasks.push_back(std::move(line));
  }
  data.hasTasks = true;
  return true;
}

// One drawn line of the scrolling body.
struct Line {
  int font;
  EpdFontFamily::Style style;
  bool section;  // bold title with a rule under it
  int indent;
  std::string left;
  std::string right;
};

void addWrapped(GfxRenderer& renderer, std::vector<Line>& lines, int font, EpdFontFamily::Style style, int width,
                const char* prefix, const std::string& text) {
  const int prefixWidth = renderer.getTextWidth(font, prefix, style) + 6;
  const auto wrapped = renderer.wrappedText(font, text.c_str(), width - prefixWidth, MAX_TASK_LINES, style);
  bool first = true;
  for (const auto& piece : wrapped) {
    lines.push_back(
        {font, style, false, first ? 0 : prefixWidth, first ? std::string(prefix) + " " + piece : piece, ""});
    first = false;
  }
  if (wrapped.empty()) lines.push_back({font, style, false, 0, prefix, ""});
}

int lineHeight(GfxRenderer& renderer, const Line& line) {
  return line.section ? 10 + renderer.getLineHeight(line.font) + 2 + 1 + 8 : renderer.getLineHeight(line.font) + 4;
}

void drawLine(GfxRenderer& renderer, const Line& line, int y, int width) {
  if (line.section) {
    y += 10;
    renderer.drawText(line.font, SIDE_PADDING, y, line.left.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(line.font) + 2;
    renderer.fillRect(SIDE_PADDING, y, width, 1, true);
    return;
  }
  const int rightWidth = line.right.empty() ? 0 : renderer.getTextWidth(line.font, line.right.c_str(), line.style);
  const std::string leftText =
      renderer.truncatedText(line.font, line.left.c_str(), width - line.indent - rightWidth - 12, line.style);
  renderer.drawText(line.font, SIDE_PADDING + line.indent, y, leftText.c_str(), true, line.style);
  if (rightWidth > 0)
    renderer.drawText(line.font, SIDE_PADDING + width - rightWidth, y, line.right.c_str(), true, line.style);
}
}  // namespace

bool configExists() { return Storage.exists(CONFIG_PATH); }

Config loadConfig() {
  Config config;
  readKeyValues(CONFIG_PATH, [&config](const std::string& key, const std::string& value) {
    if (key == "enabled")
      config.enabled = value == "1" || value == "true";
    else if (key == "tasks_url")
      config.tasksUrl = value;
    else if (key == "sleep_refresh")
      config.sleepRefresh = value == "never"    ? SleepRefresh::Never
                            : value == "always" ? SleepRefresh::Always
                                                : SleepRefresh::Stale;
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
  if (!config.tasksUrl.empty()) {
    if (fetchTasks(config, data)) {
      any = true;
    } else {
      error = tr(STR_BF_TASKS_UNAVAILABLE);
    }
  }
  if (any || (!config.hasLocation && config.tasksUrl.empty())) {
    data.fetchedAt = DateUtils::nowUtc();
    any = true;
  }
  return any;
}

bool shouldRefreshAtSleep(const Config& config, const Data& data) {
  if (!config.enabled || config.sleepRefresh == SleepRefresh::Never) return false;
  if (!config.hasLocation && config.tasksUrl.empty()) return false;
  if (powerManager.getBatteryPercentage() < MIN_BATTERY_FOR_FETCH) return false;
  if (config.sleepRefresh == SleepRefresh::Always) return true;
  if (data.fetchedAt == 0) return true;          // nothing cached yet
  if (!DateUtils::hasValidTime()) return false;  // clock reset by the last power-off: age unknown, keep the cache
  const uint32_t now = DateUtils::nowUtc();
  return now < data.fetchedAt || now - data.fetchedAt >= SLEEP_REFRESH_INTERVAL_S;
}

Page render(GfxRenderer& renderer, const Config& config, const Data& data, int bottomInset, int scroll) {
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

  std::vector<Line> lines;
  lines.reserve(48);

  // Tasks
  if (!config.tasksUrl.empty()) {
    lines.push_back({UI_12_FONT_ID, EpdFontFamily::BOLD, true, 0, tr(STR_BF_TODAY), ""});
    if (!data.hasTasks) {
      lines.push_back({UI_10_FONT_ID, EpdFontFamily::REGULAR, false, 0, tr(STR_BF_TASKS_UNAVAILABLE), ""});
    } else if (data.tasks.empty()) {
      lines.push_back({UI_10_FONT_ID, EpdFontFamily::REGULAR, false, 0, tr(STR_BF_NO_TASKS), ""});
    } else {
      for (const auto& task : data.tasks) {
        const bool overdue = task.rfind("! ", 0) == 0;
        addWrapped(renderer, lines, NOTOSANS_14_FONT_ID, overdue ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, width,
                   overdue ? "!" : "\xE2\x80\xA2", overdue ? task.substr(2) : task);
      }
    }
  }

  // Habits
  {
    HabitStore habits;
    habits.load(HabitsActivity::STORE_PATH);
    if (habits.count() > 0) {
      lines.push_back({UI_12_FONT_ID, EpdFontFamily::BOLD, true, 0, tr(STR_HABITS), ""});
      const uint32_t today = DateUtils::todayIndex();
      for (uint8_t i = 0; i < habits.count(); i++) {
        const uint16_t streak = habits.currentStreak(i, today);
        snprintf(buf, sizeof(buf), "%u %s", streak, tr(STR_HB_DAYS));
        lines.push_back({UI_12_FONT_ID, EpdFontFamily::REGULAR, false, 0,
                         std::string(habits.isDone(i, today) ? "[x] " : "[  ] ") + habits.name(i),
                         streak > 0 ? buf : ""});
      }
    }
  }

  // Flashcards
  {
    FlashcardDeck deck;
    if (deck.open(FlashcardsActivity::DECK_PATH)) {
      FlashcardScheduler scheduler;
      if (scheduler.init(deck.count(), FlashcardsActivity::PROGRESS_PATH)) {
        const auto counts = scheduler.counts();
        lines.push_back({UI_12_FONT_ID, EpdFontFamily::BOLD, true, 0, tr(STR_FLASHCARDS), ""});
        snprintf(buf, sizeof(buf), "%s %u  (%s %u, %s %u)", tr(STR_FC_DUE),
                 counts.newAvailable + counts.learningDue + counts.reviewDue, tr(STR_FC_NEW), counts.newAvailable,
                 tr(STR_FC_REVIEW), counts.reviewDue);
        lines.push_back({UI_12_FONT_ID, EpdFontFamily::REGULAR, false, 0, buf, ""});
        scheduler.release();
      }
      deck.close();
    }
  }

  // Body: draw whole lines from the first one at or past `scroll`; stop at the first that does not fit.
  Page page;
  page.viewHeight = bottomLimit - y;
  int lineTop = 0;
  int drawY = y;
  for (const auto& line : lines) {
    const int h = lineHeight(renderer, line);
    if (lineTop >= scroll) {
      if (drawY + h > bottomLimit) {
        page.nextScroll = lineTop;
        break;
      }
      drawLine(renderer, line, drawY, width);
      drawY += h;
    }
    lineTop += h;
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
  if (page.nextScroll >= 0 || scroll > 0) {
    const char* more = page.nextScroll >= 0 ? tr(STR_BF_MORE) : tr(STR_BF_TOP);
    renderer.drawText(SMALL_FONT_ID, SIDE_PADDING + width - renderer.getTextWidth(SMALL_FONT_ID, more),
                      pageHeight - footerHeight + 6, more);
  }
  return page;
}

}  // namespace Briefing
