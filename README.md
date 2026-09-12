# Teleport

Teleport is custom firmware for the Xteink X4 e-ink reader, built for one person's daily use: reading, a GRE vocabulary deck, habit tracking, a morning briefing, and an offline reading queue. It started from [CrossPoint Apps](https://github.com/zakerytclarke/crosspoint-reader-apps), which in turn builds on [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader). The EPUB engine, fonts, library and settings come from those projects; the apps, the companion desktop app and most of the networking work here are Teleport's own.

The name: this X4 shipped with a locked USB bootloader, so firmware can only travel to it on the SD card.

<img src="./docs/images/apps/homescreen.jpg" alt="Home screen" width="50%">

## What is on the device

| App | What it does |
|---|---|
| **Flashcards** | Anki-style spaced repetition (SM-2: learning steps, graduation, ease, lapses) for one deck on the card. No clock needed: you advance the study day yourself with Day - / Day +, and the side buttons set how many new cards per day. |
| **Habits** | Daily check-ins with streaks and a 12-week grid. Bulk-edit dates from the computer with `scripts/habits_tool.py`. |
| **Briefing** | Date, weather (Open-Meteo), today's tasks from Apple Calendar and Reminders (served by Nest), habit streaks and cards due. Pages with Up/Down. Can be shown as the sleep screen and refreshed at sleep time with a configurable policy. |
| **Read Later** | A queue of web pages pushed from Nest, the RSS app or `POST /api/readlater`. The reader fetches and caches each page for offline reading. |
| **RSS** | Feed reader with offline cache. Right on a post sends it to Read Later. Feeds are cut at 512 KB so full-text Substack feeds finish. |
| **Dice** | Dice, coin, spinner and 8-ball, inherited from CrossPoint Apps. |
| Browse Files, Recent, OPDS, File Transfer, Settings | From CrossPoint. |

Removed from the upstream app set: Calculator, Weather, Chess, Sudoku, DuckDuckGo, Wikipedia. The home menu order is Flashcards, Habits, Briefing, Settings, then the rest, with File Transfer last.

Card layout: `/Books`, `/Articles`, `/Digests`, `/Papers` for reading material; `/apps/<app>/` for each app's data.

## Nest, the companion app

[Nest](https://github.com/AsrorbekQ/teleport-nest) runs on the Mac and talks to the reader's File Transfer web server over Wi-Fi. It converts web pages, documents and RSS digests to EPUB with a folder picker for each send, queues Read Later links, edits habits, feeds and the briefing config, rebuilds the flashcard deck from an Anki `.apkg`, and serves today's Calendar events and Reminders as the briefing's task list.

## Technical notes specific to this fork

- **TLS on 380 KB of RAM.** Verifying a chain against a 4096-bit root (Let's Encrypt's ISRG Root X1) runs out of memory at the handshake peak. Teleport ships its own certificate bundle (`certs/`, built by `scripts/gen_crt_bundle.py`) that includes intermediates, and a verification callback in `HttpDownloader` that accepts any bundled certificate as a trust anchor, so verification stops at the 2048-bit intermediate.
- **Heap during transfers.** With a TLS session open about 16 KB of heap remains. Nothing in a download callback may allocate; buffers are reserved before connecting.
- **Redirects and IPv4.** `esp_http_client` redirects are followed by capturing `Location` from the response headers; DNS is forced to IPv4 because the device never gets a routable IPv6 address.
- **Diagnostics without serial.** ESP-IDF logs are captured into a RAM ring buffer; "Save log" in the RSS app writes them to `/log.txt`, and crash reports land in `/crash_report.txt` on the card.
- **Bonjour.** Nest is addressed as `<mac>.local`; the firmware resolves it with mDNS so a changing DHCP address does not matter.
- **Virtual time in Flashcards.** The X4 has no battery-backed clock and powers off fully in deep sleep, so the scheduler keeps a study-day counter plus elapsed seconds folded in from `millis()`.

## Flashing

The USB bootloader on the author's unit is locked, so there is no web-flasher path here.

1. Build: `pio run -e default` (PlatformIO). The image is `.pio/build/default/firmware.bin`.
2. Copy it to the root of the SD card as `update.bin`, either by mounting the card or by uploading it while File Transfer is open: `curl -F "file=@firmware.bin;filename=update.bin" "http://crosspoint.local/upload?path=/"`.
3. Power off, then hold Power and Up until the updater runs.

If your unit's bootloader is unlocked, `pio run -t upload` over USB-C works too.

## Repository layout

- `src/activities/{flashcards,habits,briefing,readlater,rss}` — the apps
- `src/network/HttpDownloader.*` — HTTPS client with the custom trust bundle
- `scripts/anki_to_deck.py` — `.apkg` to `gre.deck`
- `scripts/habits_tool.py` — `habits.bin` to editable text and back
- `scripts/gen_crt_bundle.py` — builds the certificate bundle at compile time
- `scripts/make_logo.py` — converts a PNG into the 1-bit boot logo
- `local/` — gitignored personal data (feeds, habits, briefing config)
- `docs/` — file formats and internals inherited from CrossPoint

Developer rules for the ESP32-C3 constraints are in [CLAUDE.md](./CLAUDE.md).

## License and credits

MIT, as inherited. Copyright for the CrossPoint Reader engine belongs to Dave Allie and contributors; CrossPoint Apps by Zakery Clarke and contributors. Teleport's additions are © Asrorbek Qalandarov. Not affiliated with Xteink.
