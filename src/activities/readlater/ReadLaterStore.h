#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Queue of web pages to read offline: /apps/readlater/queue.txt holds one
// "url<TAB>title" per line; fetched pages are cached next to it as HTML.
class ReadLaterStore {
 public:
  static constexpr const char* DIR = "/apps/readlater";
  static constexpr const char* QUEUE_PATH = "/apps/readlater/queue.txt";
  static constexpr size_t MAX_ENTRIES = 40;
  static constexpr size_t MAX_PAGE_BYTES = 400 * 1024;  // TxtReader holds the file in RAM; larger pages crash it

  struct Entry {
    std::string url;
    std::string title;
    bool cached = false;
  };

  bool load();
  bool save() const;
  static bool appendUrl(const std::string& url, const std::string& title);  // used by the web server

  size_t count() const { return entries.size(); }
  const Entry& at(size_t index) const { return entries[index]; }
  size_t cachedCount() const;

  static std::string cachePath(const std::string& url);
  std::string displayName(size_t index) const;

  // Downloads the page into its cache file (capped at MAX_PAGE_BYTES) and fills in a
  // missing title from the page's <title>. Requires Wi-Fi to be connected already.
  bool fetch(size_t index, std::string& error);
  void remove(size_t index);

 private:
  std::vector<Entry> entries;
};
