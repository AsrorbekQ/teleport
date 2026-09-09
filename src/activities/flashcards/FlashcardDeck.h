#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

// Reads the compact deck file produced by scripts/anki_to_deck.py.
// Only one card's text lives in RAM at a time; the caller owns the payload buffer.
class FlashcardDeck {
 public:
  static constexpr uint8_t FIELD_COUNT = 5;
  static constexpr size_t MAX_PAYLOAD = 1536;  // must match anki_to_deck.py
  static constexpr uint8_t FILE_VERSION = 1;

  enum Field : uint8_t { WORD = 0, DEFINITION_1 = 1, DEFINITION_2 = 2, EXAMPLE = 3, SYNONYMS = 4 };

  struct Card {
    uint8_t setId = 0;
    const char* fields[FIELD_COUNT] = {"", "", "", "", ""};
  };

  bool open(const char* path);
  void close();
  bool isOpen() const { return file.isOpen(); }
  uint16_t count() const { return cardCount; }

  // payload must hold MAX_PAYLOAD bytes; card.fields point into it after a successful load.
  bool loadCard(uint16_t index, uint8_t* payload, Card& card);

 private:
  HalFile file;
  uint16_t cardCount = 0;
  uint32_t tableOffset = 0;
};
