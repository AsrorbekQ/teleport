# Teleport


**Teleport** is a personal fork of [CrossPoint Apps](https://github.com/zakerytclarke/crosspoint-reader-apps), itself a community-driven fork of the original [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) project. While the upstream project focuses solely on e-reading, this fork expands the capabilities of the Xteink X4 device by supporting a robust ecosystem of **apps and utilities**. 

Our goal is to make the device more useful in your day-to-day life without compromising its battery life, stability, or its core mission as an distraction-free e-ink reader.

**Now running on:** ESP32C3-based Xteink [X4](https://www.xteink.com/products/xteink-x4) and [X3](https://www.xteink.com/products/xteink-x3).


<img src="./docs/images/apps/homescreen.jpg" alt="Home screen" width="50%">

## Features

In addition to all the fantastic EPUB rendering, custom fonts, and library management features from the upstream CrossPoint project, **Teleport** includes a suite of applications and technical capabilities. Its companion app, [Nest](https://github.com/AsrorbekQ/teleport-nest), converts web pages, documents and RSS feeds to EPUB on your computer and syncs them over Wi-Fi.

- **Markdown & HTML Parser**: Features a custom parser and renderer that gracefully strips HTML tags and translates basic Markdown, allowing web content (like RSS articles and Reddit) to be displayed elegantly in the native text reader engine.
- **Flashcards**: Anki-style spaced repetition (SM-2) for a vocabulary deck stored on the SD card. Convert an `.apkg` export with `scripts/anki_to_deck.py` and copy the result to `/apps/flashcards/gre.deck`.
- **Habits**: Daily habit check-ins with streaks and a 12-week completion grid. Fully offline; only needs the clock synced once.
- **Read Later**: Queue web pages from [Nest](https://github.com/AsrorbekQ/teleport-nest) (or `POST /api/readlater`); the device fetches and caches them for offline reading.
- **Briefing**: A morning dashboard with the date, weather (Open-Meteo), today's Todoist tasks, habit streaks and flashcards due. Optionally fetched at sleep time and left on the e-ink screen overnight. Configure it from Nest.
- **Dice & 8-Ball**: A handy utility for tabletop gamers. Roll D6, D20, spin arrows, flip coins, or consult the Magic 8-Ball.
- **RSS Feed & Reddit**: Subscribe to your favorite blogs and news sites. Articles are downloaded and cached for distraction-free, offline reading using the native text reader engine.


## Gallery
<table width="100%">
  <tr>
    <td><img src="./docs/images/apps/rss.png" alt="RSS" width="100%"></td>
    <td><img src="./docs/images/apps/reddit.png" alt="Reddit" width="100%"></td>
  </tr>
  <tr>
    <td><img src="./docs/images/apps/markdown.png" alt="Markdown" width="100%"></td>
    <td><img src="./docs/images/apps/html.png" alt="HTML" width="100%"></td>
    <td><img src="./docs/images/apps/dice.png" alt="Dice" width="100%"></td>
  </tr>
</table>




## Install Firmware

### Web Installer

1. Download the pre-compiled binary from this repository: [`bin/crosspoint-apps.bin`](./bin/crosspoint-apps.bin).
2. Connect your device to your computer via USB-C and wake/unlock the device.
3. Go to the [Web Flasher](https://crosspointreader.com/#flash-tools), select your device (X3 or X4).
4. Click "Custom .bin" and upload the `crosspoint-apps.bin` file you just downloaded.

### Development Quick Start / Command Line

1. Clone this repository:
```bash
git clone --recursive https://github.com/AsrorbekQ/teleport.git
cd teleport
```

2. Install PlatformIO (if you haven't already).
3. Connect your device via USB-C.
4. Build and flash the firmware:
```bash
pio run --target upload
```

## Community Contributions

We enthusiastically welcome contributions! If you have an idea for an app that would be useful on an e-ink device, we want it. 

We strongly encourage apps that are **offline-first**—meaning they do not require a constant Wi-Fi connection to function. Use the SD card to cache data (like Reddit posts or RSS feeds) so users can refresh their feeds once and read them anywhere.

See [CLAUDE.md](./CLAUDE.md) for detailed developer guidelines on how to build apps that respect the constraints of the ESP32-C3 and the e-ink display.

## Documentation

- [User Guide](./USER_GUIDE.md) - Learn how to use the device and the new apps.
- [Project Scope](./SCOPE.md) - Understand our philosophy on what makes a good app.
- [Developer Guidelines](./CLAUDE.md) - Read this before contributing code or new apps!

---

Teleport is a personal fork and is **not affiliated with Xteink or any device manufacturer**.
