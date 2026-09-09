#include "ReadLaterStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>

#include "network/HttpDownloader.h"

namespace {
constexpr const char* TAG = "READLATER";
constexpr size_t TITLE_SCAN_BYTES = 8192;

void trimLine(std::string& line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
}

// Pulls the text of the first <title> element out of the head of a page.
std::string extractTitle(const std::string& head) {
  size_t open = head.find("<title");
  if (open == std::string::npos) return "";
  open = head.find('>', open);
  if (open == std::string::npos) return "";
  const size_t close = head.find("</title>", open);
  if (close == std::string::npos) return "";
  std::string title = head.substr(open + 1, close - open - 1);
  std::string cleaned;
  cleaned.reserve(title.size());
  bool space = true;
  for (char c : title) {
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      if (!space) cleaned.push_back(' ');
      space = true;
    } else {
      cleaned.push_back(c);
      space = false;
    }
  }
  trimLine(cleaned);
  if (cleaned.size() > 120) cleaned.resize(120);
  return cleaned;
}
}  // namespace

std::string ReadLaterStore::cachePath(const std::string& url) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%s/%08x.html", DIR, static_cast<unsigned>(std::hash<std::string>{}(url)));
  return buf;
}

bool ReadLaterStore::load() {
  entries.clear();
  entries.reserve(MAX_ENTRIES);
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists(DIR);

  HalFile file;
  if (!Storage.openFileForRead(TAG, QUEUE_PATH, file)) return true;

  std::string line;
  while (file.available() > 0 && entries.size() < MAX_ENTRIES) {
    const char c = static_cast<char>(file.read());
    if (c != '\n') {
      line.push_back(c);
      continue;
    }
    trimLine(line);
    if (!line.empty() && line[0] != '#') {
      Entry entry;
      const size_t tab = line.find('\t');
      entry.url = line.substr(0, tab);
      if (tab != std::string::npos) entry.title = line.substr(tab + 1);
      entry.cached = Storage.exists(cachePath(entry.url).c_str());
      entries.push_back(std::move(entry));
    }
    line.clear();
  }
  trimLine(line);
  if (!line.empty() && line[0] != '#' && entries.size() < MAX_ENTRIES) {
    Entry entry;
    const size_t tab = line.find('\t');
    entry.url = line.substr(0, tab);
    if (tab != std::string::npos) entry.title = line.substr(tab + 1);
    entry.cached = Storage.exists(cachePath(entry.url).c_str());
    entries.push_back(std::move(entry));
  }
  return true;
}

bool ReadLaterStore::save() const {
  HalFile file;
  if (!Storage.openFileForWrite(TAG, QUEUE_PATH, file)) {
    LOG_ERR(TAG, "Cannot write queue");
    return false;
  }
  for (const auto& entry : entries) {
    file.write(entry.url.data(), entry.url.size());
    if (!entry.title.empty()) {
      file.write("\t", 1);
      file.write(entry.title.data(), entry.title.size());
    }
    file.write("\n", 1);
  }
  return true;
}

bool ReadLaterStore::appendUrl(const std::string& url, const std::string& title) {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists(DIR);
  std::string existing;
  {
    HalFile file;
    if (Storage.openFileForRead(TAG, QUEUE_PATH, file)) {
      existing.reserve(file.size() + url.size() + title.size() + 2);
      while (file.available() > 0) existing.push_back(static_cast<char>(file.read()));
    }
  }
  if (existing.find(url) != std::string::npos) return true;  // already queued
  if (!existing.empty() && existing.back() != '\n') existing.push_back('\n');
  existing += url;
  if (!title.empty()) existing += "\t" + title;
  existing.push_back('\n');

  HalFile file;
  if (!Storage.openFileForWrite(TAG, QUEUE_PATH, file)) return false;
  return file.write(existing.data(), existing.size()) == existing.size();
}

size_t ReadLaterStore::cachedCount() const {
  size_t n = 0;
  for (const auto& entry : entries) n += entry.cached ? 1 : 0;
  return n;
}

std::string ReadLaterStore::displayName(size_t index) const {
  const Entry& entry = entries[index];
  if (!entry.title.empty()) return entry.title;
  std::string name = entry.url;
  const size_t scheme = name.find("://");
  if (scheme != std::string::npos) name.erase(0, scheme + 3);
  if (name.rfind("www.", 0) == 0) name.erase(0, 4);
  return name;
}

bool ReadLaterStore::fetch(size_t index, std::string& error) {
  if (index >= entries.size()) return false;
  Entry& entry = entries[index];
  const std::string path = cachePath(entry.url);
  const std::string tempPath = std::string(DIR) + "/download.tmp";
  Storage.remove(tempPath.c_str());

  HalFile file;
  if (!Storage.openFileForWrite(TAG, tempPath.c_str(), file)) {
    error = "Cannot write cache";
    return false;
  }

  size_t written = 0;
  std::string head;
  bool truncated = false;
  const bool ok = HttpDownloader::fetchUrl(entry.url, [&](const uint8_t* data, size_t len) {
    if (head.size() < TITLE_SCAN_BYTES) {
      head.append(reinterpret_cast<const char*>(data), std::min(len, TITLE_SCAN_BYTES - head.size()));
    }
    if (written + len > MAX_PAGE_BYTES) {
      len = MAX_PAGE_BYTES - written;
      truncated = true;
    }
    if (len > 0 && file.write(data, len) != len) return false;
    written += len;
    return !truncated;
  });
  file.close();

  // A page cut at the size cap is still worth reading; only a real HTTP failure with nothing saved is an error.
  if (!ok && !truncated && written == 0) {
    Storage.remove(tempPath.c_str());
    error = "Download failed";
    return false;
  }
  Storage.remove(path.c_str());
  if (!Storage.rename(tempPath.c_str(), path.c_str())) {
    error = "Cannot save page";
    return false;
  }
  if (truncated)
    LOG_INF(TAG, "Page truncated at %u bytes: %s", static_cast<unsigned>(MAX_PAGE_BYTES), entry.url.c_str());

  entry.cached = true;
  if (entry.title.empty()) {
    const std::string title = extractTitle(head);
    if (!title.empty()) entry.title = title;
  }
  return true;
}

void ReadLaterStore::remove(size_t index) {
  if (index >= entries.size()) return;
  Storage.remove(cachePath(entries[index].url).c_str());
  entries.erase(entries.begin() + static_cast<long>(index));
}
