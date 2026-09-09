#include "FlashcardDeck.h"

#include <Logging.h>

#include <cstring>

namespace {
constexpr const char* TAG = "FCDECK";
constexpr size_t HEADER_SIZE = 16;
}  // namespace

bool FlashcardDeck::open(const char* path) {
  close();
  if (!Storage.openFileForRead(TAG, path, file)) {
    LOG_ERR(TAG, "Deck not found: %s", path);
    return false;
  }

  uint8_t header[HEADER_SIZE];
  if (file.read(header, HEADER_SIZE) != static_cast<int>(HEADER_SIZE) || memcmp(header, "CPFC", 4) != 0) {
    LOG_ERR(TAG, "Bad deck header");
    close();
    return false;
  }
  if (header[4] != FILE_VERSION || header[5] != FIELD_COUNT) {
    LOG_ERR(TAG, "Unsupported deck version %u/%u", header[4], header[5]);
    close();
    return false;
  }
  memcpy(&cardCount, header + 6, sizeof(cardCount));
  memcpy(&tableOffset, header + 8, sizeof(tableOffset));
  if (cardCount == 0) {
    LOG_ERR(TAG, "Deck is empty");
    close();
    return false;
  }
  LOG_INF(TAG, "Opened deck with %u cards", cardCount);
  return true;
}

void FlashcardDeck::close() {
  if (file.isOpen()) {
    file.close();
  }
  cardCount = 0;
}

bool FlashcardDeck::loadCard(uint16_t index, uint8_t* payload, Card& card) {
  if (!file.isOpen() || index >= cardCount) {
    return false;
  }

  uint32_t recordOffset = 0;
  if (!file.seek(tableOffset + static_cast<uint32_t>(index) * 4) ||
      file.read(&recordOffset, sizeof(recordOffset)) != static_cast<int>(sizeof(recordOffset))) {
    LOG_ERR(TAG, "Offset read failed for card %u", index);
    return false;
  }

  uint8_t recordHeader[3];
  if (!file.seek(recordOffset) ||
      file.read(recordHeader, sizeof(recordHeader)) != static_cast<int>(sizeof(recordHeader))) {
    LOG_ERR(TAG, "Record read failed for card %u", index);
    return false;
  }
  card.setId = recordHeader[0];
  uint16_t length = 0;
  memcpy(&length, recordHeader + 1, sizeof(length));
  if (length >= MAX_PAYLOAD) {
    length = MAX_PAYLOAD - 1;
  }
  if (file.read(payload, length) != static_cast<int>(length)) {
    LOG_ERR(TAG, "Payload read failed for card %u", index);
    return false;
  }
  payload[length] = 0;

  const char* cursor = reinterpret_cast<const char*>(payload);
  const char* end = cursor + length;
  for (uint8_t i = 0; i < FIELD_COUNT; i++) {
    if (cursor < end) {
      card.fields[i] = cursor;
      cursor += strlen(cursor) + 1;
    } else {
      card.fields[i] = end;  // points at the terminating NUL: empty string
    }
  }
  return true;
}
