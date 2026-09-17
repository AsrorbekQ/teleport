#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <FsHelpers.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <Serialization.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "ProgressFile.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 3;          // Increment when cache format changes
}  // namespace

bool TxtReaderActivity::loadBook() {
  txt = makeUniqueNoThrow<Txt>(bookPath, "/.crosspoint");
  if (!txt) {
    LOG_ERR("TRS", "Failed to allocate TXT object");
    return false;
  }
  if (!txt->load()) {
    LOG_ERR("TRS", "Failed to load TXT");
    return false;
  }
  txt->setupCacheDir();
  return true;
}

void TxtReaderActivity::initializeReader(GfxRenderer& renderer) {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    buildPageIndex(renderer);
    // Save to cache for next time
    savePageIndexCache();
  }

  // Load saved progress
  loadProgress();

  initialized = true;
}

void TxtReaderActivity::buildPageIndex(GfxRenderer& renderer) {
  pageOffsets.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  GUI.drawPopup(renderer, tr(STR_INDEXING));

  while (offset < fileSize) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(renderer, offset, tempLines, nextOffset)) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    // Yield to other tasks periodically
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Built page index: %d pages", totalPages);
}

bool TxtReaderActivity::isHtmlFile() const {
  return txt && (FsHelpers::checkFileExtension(txt->getPath(), ".html") ||
                 FsHelpers::checkFileExtension(txt->getPath(), ".htm"));
}

size_t TxtReaderActivity::wrapAndPushHtmlLine(const GfxRenderer& renderer, const std::string& line, const char marker,
                                              const EpdFontFamily::Style style, const int indent,
                                              std::vector<std::string>& outLines) {
  std::string cleanLine = line;
  bool firstSegment = true;
  size_t charsConsumed = 0;

  while (!cleanLine.empty() && static_cast<int>(outLines.size()) < linesPerPage) {
    int currentIndent = firstSegment ? indent : (marker == '\5' ? 15 : indent);
    int maxW = viewportWidth - currentIndent;

    int lineWidth = renderer.getTextAdvanceX(cachedFontId, cleanLine.c_str(), style);

    if (lineWidth <= maxW) {
      std::string wrapped = "";
      wrapped += (firstSegment ? marker : (marker == '\5' ? '\4' : marker));
      wrapped += cleanLine;
      outLines.push_back(wrapped);
      charsConsumed += cleanLine.length();
      break;
    }

    // Find break point using binary search
    size_t low = 0;
    size_t high = cleanLine.length();
    size_t breakPos = 0;

    while (low <= high) {
      size_t mid = low + (high - low) / 2;
      while (mid > low && (cleanLine[mid] & 0xC0) == 0x80) {
        mid--;
      }

      std::string testStr = cleanLine.substr(0, mid);
      int testWidth = renderer.getTextAdvanceX(cachedFontId, testStr.c_str(), style);

      if (testWidth <= maxW) {
        breakPos = mid;
        low = mid + 1;
        while (low <= high && low < cleanLine.length() && (cleanLine[low] & 0xC0) == 0x80) {
          low++;
        }
      } else {
        if (mid == 0) {
          breakPos = 0;
          break;
        }
        high = mid - 1;
      }
    }

    if (breakPos == 0) {
      breakPos = 1;
      while (breakPos < cleanLine.length() && (cleanLine[breakPos] & 0xC0) == 0x80) {
        breakPos++;
      }
    }

    if (breakPos < cleanLine.length()) {
      size_t spacePos = cleanLine.rfind(' ', breakPos);
      if (spacePos != std::string::npos && spacePos > 0) {
        if (spacePos > breakPos - 20 || spacePos > cleanLine.length() / 2) {
          breakPos = spacePos;
        }
      }
    }

    std::string wrapped = "";
    wrapped += (firstSegment ? marker : (marker == '\5' ? '\4' : marker));
    wrapped += cleanLine.substr(0, breakPos);
    outLines.push_back(wrapped);

    size_t skipChars = breakPos;
    if (breakPos < cleanLine.length() && cleanLine[breakPos] == ' ') {
      skipChars++;
    }

    charsConsumed += skipChars;
    cleanLine = cleanLine.substr(skipChars);
    firstSegment = false;
  }

  return charsConsumed;
}

bool TxtReaderActivity::loadPageAtOffset(const GfxRenderer& renderer, size_t offset, std::vector<std::string>& outLines,
                                         size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read Later caches fetched articles as .html next to its queue; the plain-text path below
  // would render their markup verbatim, so strip tags into marker-prefixed lines instead.
  if (isHtmlFile()) {
    size_t currentOffset = offset;
    size_t bufferPos = 0;
    size_t bytesReadInChunk = 0;
    auto* buffer = static_cast<uint8_t*>(malloc(CHUNK_SIZE + 1));
    if (!buffer) {
      LOG_ERR("TRS", "Failed to allocate %zu bytes", CHUNK_SIZE);
      return false;
    }

    auto getChar = [&]() -> int {
      if (bufferPos >= bytesReadInChunk) {
        if (currentOffset >= fileSize) return -1;
        size_t toRead = std::min(CHUNK_SIZE, fileSize - currentOffset);
        if (!txt->readContent(buffer, currentOffset, toRead)) return -1;
        buffer[toRead] = '\0';

        if (renderer.isSdCardFont(cachedFontId)) {
          renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), 0x01);
        }

        bytesReadInChunk = toRead;
        currentOffset += toRead;
        bufferPos = 0;
      }
      return buffer[bufferPos++];
    };

    auto getFilePos = [&]() -> size_t { return currentOffset - bytesReadInChunk + bufferPos - 1; };

    std::string cleanLine = "";
    std::vector<size_t> cleanLineOffsets;
    char marker = '\7';
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    int indent = 0;
    bool lastWasSpace = true;
    bool insideBlockquote = false;

    int c;
    while (static_cast<int>(outLines.size()) < linesPerPage && (c = getChar()) != -1) {
      if (c == '<') {
        int c1 = getChar();
        if (c1 == -1) break;

        if (c1 == '!') {
          int c2 = getChar();
          int c3 = getChar();
          if (c2 == '-' && c3 == '-') {
            // HTML Comment
            int dashCount = 0;
            int cm;
            while ((cm = getChar()) != -1) {
              if (cm == '-')
                dashCount++;
              else if (cm == '>' && dashCount >= 2)
                break;
              else
                dashCount = 0;
            }
            continue;
          } else {
            // Not a comment, ignore DOCTYPE etc.
            int cm;
            while ((cm = getChar()) != -1 && cm != '>') {
            }
            continue;
          }
        }

        std::string tagContent;
        tagContent += static_cast<char>(c1);
        int tc;
        while ((tc = getChar()) != -1 && tc != '>') {
          if (tagContent.length() < 256) tagContent += static_cast<char>(tc);
        }

        std::string tagName = "";
        size_t firstSpace = tagContent.find_first_of(" \t\r\n/");
        if (firstSpace != std::string::npos) {
          tagName = tagContent.substr(0, firstSpace);
        } else {
          tagName = tagContent;
        }
        std::transform(tagName.begin(), tagName.end(), tagName.begin(), ::tolower);

        bool isClosing = (!tagContent.empty() && tagContent[0] == '/');
        if (isClosing && !tagName.empty() && tagName[0] == '/') {
          tagName = tagName.substr(1);
        }

        // Fast path for skipping content tags
        if (!isClosing && (tagName == "style" || tagName == "script" || tagName == "head" || tagName == "svg" ||
                           tagName == "nav" || tagName == "noscript" || tagName == "iframe")) {
          std::string closeTag = "</" + tagName + ">";
          size_t matchIdx = 0;
          int sc;
          while ((sc = getChar()) != -1) {
            if (tolower(sc) == closeTag[matchIdx]) {
              matchIdx++;
              if (matchIdx == closeTag.length()) break;
            } else {
              if (tolower(sc) == closeTag[0])
                matchIdx = 1;
              else
                matchIdx = 0;
            }
          }
          continue;
        }

        // Structural tags
        if (tagName == "h1") {
          if (!cleanLine.empty()) {
            size_t consumed = wrapAndPushHtmlLine(renderer, cleanLine, marker, style, indent, outLines);
            if (consumed < cleanLine.length()) {
              nextOffset = cleanLineOffsets[consumed];
              free(buffer);
              return true;
            }
            cleanLine.clear();
            cleanLineOffsets.clear();
          }
          marker = isClosing ? '\7' : '\1';
          style = isClosing ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
          indent = 0;
        } else if (tagName == "h2") {
          if (!cleanLine.empty()) {
            size_t consumed = wrapAndPushHtmlLine(renderer, cleanLine, marker, style, indent, outLines);
            if (consumed < cleanLine.length()) {
              nextOffset = cleanLineOffsets[consumed];
              free(buffer);
              return true;
            }
            cleanLine.clear();
            cleanLineOffsets.clear();
          }
          marker = isClosing ? '\7' : '\2';
          style = isClosing ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
          indent = 0;
        } else if (tagName == "h3") {
          if (!cleanLine.empty()) {
            size_t consumed = wrapAndPushHtmlLine(renderer, cleanLine, marker, style, indent, outLines);
            if (consumed < cleanLine.length()) {
              nextOffset = cleanLineOffsets[consumed];
              free(buffer);
              return true;
            }
            cleanLine.clear();
            cleanLineOffsets.clear();
          }
          marker = isClosing ? '\7' : '\3';
          style = isClosing ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
          indent = 0;
        } else if (tagName == "blockquote") {
          if (!cleanLine.empty()) {
            size_t consumed = wrapAndPushHtmlLine(renderer, cleanLine, marker, style, indent, outLines);
            if (consumed < cleanLine.length()) {
              nextOffset = cleanLineOffsets[consumed];
              free(buffer);
              return true;
            }
            cleanLine.clear();
            cleanLineOffsets.clear();
          }
          insideBlockquote = !isClosing;
          marker = isClosing ? '\7' : '\4';
          style = isClosing ? EpdFontFamily::REGULAR : EpdFontFamily::ITALIC;
          indent = isClosing ? 0 : 15;
        } else if (tagName == "li") {
          if (!cleanLine.empty()) {
            size_t consumed = wrapAndPushHtmlLine(renderer, cleanLine, marker, style, indent, outLines);
            if (consumed < cleanLine.length()) {
              nextOffset = cleanLineOffsets[consumed];
              free(buffer);
              return true;
            }
            cleanLine.clear();
            cleanLineOffsets.clear();
          }
          if (!isClosing) {
            marker = '\5';
            indent = 15;
            cleanLine = "•  ";
            cleanLineOffsets.insert(cleanLineOffsets.end(), 3, getFilePos());
          } else {
            marker = '\7';
            indent = 0;
          }
        } else if (tagName == "hr") {
          if (!cleanLine.empty()) {
            size_t consumed = wrapAndPushHtmlLine(renderer, cleanLine, marker, style, indent, outLines);
            if (consumed < cleanLine.length()) {
              nextOffset = cleanLineOffsets[consumed];
              free(buffer);
              return true;
            }
            cleanLine.clear();
            cleanLineOffsets.clear();
          }
          std::string hrStr = "";
          hrStr += '\6';
          outLines.push_back(hrStr);
        } else if (tagName == "p" || tagName == "div" || tagName == "br") {
          if (!cleanLine.empty()) {
            size_t consumed = wrapAndPushHtmlLine(renderer, cleanLine, marker, style, indent, outLines);
            if (consumed < cleanLine.length()) {
              nextOffset = cleanLineOffsets[consumed];
              free(buffer);
              return true;
            }
            cleanLine.clear();
            cleanLineOffsets.clear();
          }
          if (insideBlockquote) {
            marker = '\4';
            style = EpdFontFamily::ITALIC;
            indent = 15;
          } else {
            marker = '\7';
            style = EpdFontFamily::REGULAR;
            indent = 0;
          }
        }

        lastWasSpace = true;
        continue;
      }

      // Handle Entities
      if (c == '&') {
        size_t entityStartPos = getFilePos();
        std::string entity;
        int ec;
        bool foundSemi = false;
        int ahead[10];
        int aheadCount = 0;
        while (aheadCount < 10 && (ec = getChar()) != -1) {
          ahead[aheadCount++] = ec;
          if (ec == ';') {
            foundSemi = true;
            break;
          }
        }

        if (foundSemi) {
          for (int k = 0; k < aheadCount - 1; k++) entity += static_cast<char>(ahead[k]);
          int code = 0;
          bool decoded = false;

          if (!entity.empty() && entity[0] == '#') {
            decoded = true;
            if (entity.length() > 2 && (entity[1] == 'x' || entity[1] == 'X')) {
              for (size_t j = 2; j < entity.length(); j++) {
                char ch = entity[j];
                if (ch >= '0' && ch <= '9')
                  code = code * 16 + (ch - '0');
                else if (ch >= 'a' && ch <= 'f')
                  code = code * 16 + (ch - 'a' + 10);
                else if (ch >= 'A' && ch <= 'F')
                  code = code * 16 + (ch - 'A' + 10);
              }
            } else {
              for (size_t j = 1; j < entity.length(); j++) {
                char ch = entity[j];
                if (ch >= '0' && ch <= '9') code = code * 10 + (ch - '0');
              }
            }
          } else {
            if (entity == "nbsp") {
              code = 32;
              decoded = true;
            } else if (entity == "amp") {
              code = 38;
              decoded = true;
            } else if (entity == "lt") {
              code = 60;
              decoded = true;
            } else if (entity == "gt") {
              code = 62;
              decoded = true;
            } else if (entity == "quot") {
              code = 34;
              decoded = true;
            } else if (entity == "apos" || entity == "#39") {
              code = 39;
              decoded = true;
            } else if (entity == "ldquo") {
              code = 8220;
              decoded = true;
            } else if (entity == "rdquo") {
              code = 8221;
              decoded = true;
            } else if (entity == "lsquo") {
              code = 8216;
              decoded = true;
            } else if (entity == "rsquo") {
              code = 8217;
              decoded = true;
            } else if (entity == "ndash") {
              code = 8211;
              decoded = true;
            } else if (entity == "mdash") {
              code = 8212;
              decoded = true;
            } else if (entity == "hellip") {
              code = 8230;
              decoded = true;
            } else if (entity == "euro") {
              code = 8364;
              decoded = true;
            } else if (entity == "copy") {
              code = 169;
              decoded = true;
            } else if (entity == "reg") {
              code = 174;
              decoded = true;
            } else if (entity == "trade") {
              code = 8482;
              decoded = true;
            }
          }

          if (decoded && code > 0) {
            std::string utf8_char = "";
            if (code <= 0x7F)
              utf8_char += static_cast<char>(code);
            else if (code <= 0x7FF) {
              utf8_char += static_cast<char>(0xC0 | ((code >> 6) & 0x1F));
              utf8_char += static_cast<char>(0x80 | (code & 0x3F));
            } else if (code <= 0xFFFF) {
              utf8_char += static_cast<char>(0xE0 | ((code >> 12) & 0x0F));
              utf8_char += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
              utf8_char += static_cast<char>(0x80 | (code & 0x3F));
            } else if (code <= 0x10FFFF) {
              utf8_char += static_cast<char>(0xF0 | ((code >> 18) & 0x07));
              utf8_char += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
              utf8_char += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
              utf8_char += static_cast<char>(0x80 | (code & 0x3F));
            }

            for (char uc : utf8_char) {
              if (isspace(uc)) {
                if (!lastWasSpace) {
                  cleanLine += ' ';
                  cleanLineOffsets.push_back(entityStartPos);
                  lastWasSpace = true;
                }
              } else {
                cleanLine += uc;
                cleanLineOffsets.push_back(entityStartPos);
                lastWasSpace = false;
              }
            }
            continue;
          }
        }

        cleanLine += '&';
        cleanLineOffsets.push_back(entityStartPos);
        lastWasSpace = false;
        for (int k = 0; k < aheadCount; k++) {
          char ac = static_cast<char>(ahead[k]);
          size_t acPos = entityStartPos + 1 + k;
          if (isspace(ac)) {
            if (!lastWasSpace) {
              cleanLine += ' ';
              cleanLineOffsets.push_back(acPos);
              lastWasSpace = true;
            }
          } else {
            cleanLine += ac;
            cleanLineOffsets.push_back(acPos);
            lastWasSpace = false;
          }
        }
        continue;
      }

      if (isspace(c)) {
        if (!lastWasSpace) {
          cleanLine += ' ';
          cleanLineOffsets.push_back(getFilePos());
          lastWasSpace = true;
        }
      } else {
        cleanLine += static_cast<char>(c);
        cleanLineOffsets.push_back(getFilePos());
        lastWasSpace = false;
      }
    }

    if (!cleanLine.empty()) {
      if (static_cast<int>(outLines.size()) < linesPerPage) {
        size_t consumed = wrapAndPushHtmlLine(renderer, cleanLine, marker, style, indent, outLines);
        if (consumed < cleanLine.length()) {
          nextOffset = cleanLineOffsets[consumed];
          free(buffer);
          return true;
        }
      } else {
        nextOffset = cleanLineOffsets[0];
        free(buffer);
        return true;
      }
    }

    nextOffset = currentOffset - bytesReadInChunk + bufferPos;
    if (nextOffset > fileSize) nextOffset = fileSize;
    free(buffer);
    return !outLines.empty();
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  if (renderer.isSdCardFont(cachedFontId)) {
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), /*styleMask=*/0x01);
  }

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    size_t lineContentLen = lineEnd - pos;
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    std::string line(reinterpret_cast<char*>(buffer + pos), displayLen);
    size_t lineBytePos = 0;

    do {
      if (line.empty()) {
        outLines.emplace_back();
        break;
      }

      int lineWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

      if (lineWidth <= viewportWidth) {
        outLines.push_back(line);
        lineBytePos = displayLen;
        line.clear();
        break;
      }

      // Find break point
      size_t breakPos = line.length();
      while (breakPos > 0 && renderer.getTextAdvanceX(cachedFontId, line.substr(0, breakPos).c_str(),
                                                      EpdFontFamily::REGULAR) > viewportWidth) {
        // Try to break at space
        size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        } else {
          // Break at character boundary for UTF-8
          breakPos--;
          while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) {
            breakPos--;
          }
        }
      }

      if (breakPos == 0) {
        breakPos = 1;
      }

      outLines.push_back(line.substr(0, breakPos));

      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    } while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage);

    if (line.empty()) {
      pos = lineEnd + 1;
    } else {
      pos = pos + lineBytePos;
      break;
    }
  }

  if (pos == 0 && !outLines.empty()) {
    pos = 1;
  }

  nextOffset = offset + pos;
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  free(buffer);
  return !outLines.empty();
}

void TxtReaderActivity::renderBook() {
  if (!txt) {
    return;
  }

  if (!initialized) {
    initializeReader(renderer);
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(renderer, offset, currentPageLines, nextOffset);

  renderer.clearScreen();
  renderPage(renderer);

  // Save progress
  saveProgress();
}

void TxtReaderActivity::renderPage(GfxRenderer& renderer) {
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  // Lines produced by the HTML path carry a leading marker byte naming the block they came from
  // (heading / blockquote / bullet / rule); plain text and Markdown lines have none.
  const bool markedUp = isHtmlFile();

  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& rawLine : currentPageLines) {
      std::string line = rawLine;
      auto style = EpdFontFamily::REGULAR;
      int indent = 0;
      bool underlineHeading = false;
      bool quoteBar = false;
      bool horizontalRule = false;

      if (markedUp && !line.empty()) {
        const char kind = line[0];
        line.erase(0, 1);
        switch (kind) {
          case '\1':
            style = EpdFontFamily::BOLD;
            underlineHeading = true;
            break;
          case '\2':
          case '\3':
            style = EpdFontFamily::BOLD;
            break;
          case '\4':
            style = EpdFontFamily::ITALIC;
            indent = 15;
            quoteBar = true;
            break;
          case '\5':
            indent = 15;
            break;
          case '\6':
            horizontalRule = true;
            break;
          default:
            break;
        }
      }

      if (horizontalRule) {
        const int ruleY = y + lineHeight / 2;
        renderer.drawLine(cachedOrientedMarginLeft + 10, ruleY, cachedOrientedMarginLeft + contentWidth - 10, ruleY,
                          true);
      } else if (!line.empty()) {
        int x = cachedOrientedMarginLeft + indent;
        const bool lineIsRtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        uint8_t effectiveAlignment = cachedParagraphAlignment;
        if (lineIsRtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                          effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), style);

        // Apply text alignment
        switch (effectiveAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            x = cachedOrientedMarginLeft + indent + (contentWidth - indent - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            break;
        }

        if (quoteBar) {
          renderer.fillRect(cachedOrientedMarginLeft + 5, y, 2, lineHeight, true);
        }

        renderer.drawText(cachedFontId, x, y, line.c_str(), true, style);

        if (underlineHeading) {
          const int underlineY = y + lineHeight - 2;
          renderer.drawLine(cachedOrientedMarginLeft, underlineY, cachedOrientedMarginLeft + contentWidth, underlineY,
                            true);
        }
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();      // scan pass
  renderStatusBar();  // scan: a CJK title joins the batch prewarm
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh);
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
}

void TxtReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarSpec().showsTitle()) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

bool TxtReaderActivity::pageTurn(bool isForward) {
  // Ignore paging until initializeReader has established the page index
  if (!initialized) {
    return false;
  }
  if (isForward) {
    if (currentPage < totalPages) {
      currentPage++;
      return true;
    }
  } else {
    if (currentPage > 0) {
      currentPage--;
      return true;
    }
  }
  return false;
}

bool TxtReaderActivity::skipPages(int amount) {
  if (!initialized) {
    return false;
  }
  int newPage = currentPage + amount;
  if (newPage < 0) newPage = 0;
  // Clamp to totalPages, not totalPages - 1: pageTurn() lets currentPage reach
  // totalPages and isAtEndOfBook() treats that as the end-of-book sentinel, so
  // a forward skip must be able to reach it too.
  if (newPage > totalPages) newPage = totalPages;
  if (newPage != currentPage) {
    currentPage = newPage;
    return true;
  }
  return false;
}

bool TxtReaderActivity::isAtEndOfBook() const { return initialized && currentPage >= totalPages; }

void TxtReaderActivity::onReturnFromEndOfBook() { currentPage = totalPages > 0 ? totalPages - 1 : 0; }

void TxtReaderActivity::saveProgress() const {
  uint8_t data[4];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = 0;
  data[3] = 0;
  if (!ProgressFile::writeAtomic(txt->getCachePath(), data, sizeof(data))) {
    LOG_ERR("TRS", "Failed to save progress: page %d", currentPage);
  }
}

void TxtReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
