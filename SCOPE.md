# Project Vision & Scope: Teleport

The goal of **Teleport** is to create an efficient, open-source reading experience for the Xteink X4, augmented by a small suite of high-quality applications. We believe an e-reader can be a powerful, distraction-free companion device that goes beyond just reading books—provided the apps are designed with care for the e-ink display and battery life.

Teleport is a fork. The reading engine comes from [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader); the app layer and the companion desktop app are ours. The two projects have deliberately different scopes, and section 5 spells out where they diverge.

## 1. Core Mission

To provide a lightweight, high-performance firmware that maximizes the utility of the X4. We aim to support a small, curated set of apps and tools while maintaining the device's exceptional legibility, long battery life, and distraction-free nature.

## 2. Scope

### In-Scope

*These are features and apps that directly improve the utility of the device while respecting its hardware constraints.*

* **Core Reading Experience:** The CrossPoint reading engine as forked — EPUB 2/3 rendering, typography, hyphenation, kerning, tables, footnotes, dictionary lookup, bookmarks, progress sync, library management, and local file transfer.
* **Productivity Utilities:** Offline tools that earn their place on a secondary, distraction-free screen — habit tracking, spaced-repetition flashcards, the sleep-screen briefing, clocks and dice rollers.
* **Offline-First Feeds & Articles:** RSS and Read Later. These MUST fetch via Wi-Fi only when requested, cache the text locally to the SD card, and then disconnect from the network to allow for battery-friendly offline reading.
* **Reference Tools:** Local dictionary lookup, offline documentation viewers, etc.
* **Memory and Flash Discipline:** Refactors and cleanups that reduce resource use, even without a user-visible feature. The ESP32-C3's ~380 KB of RAM is the binding constraint on everything above.

### Out-of-Scope

*These items are rejected because they compromise the device's stability, battery life, or e-ink constraints.*

* **High-Framerate / Animated Games:** E-ink displays have a slow refresh rate. Action games, platformers, or anything requiring rapid screen updates are fundamentally incompatible with this hardware.
* **Apps That Do Not Earn Their Flash:** The image sits close to the ceiling of the 6.5 MB app partition. Calculator, Weather, Chess, Sudoku, Wikipedia and DuckDuckGo were removed for this reason; new apps have to displace something.
* **Always-Online / Background Polling Apps:** Background Wi-Fi tasks rapidly drain the small battery and complicate the single-core CPU's execution. Apps must not poll servers continuously in the background or require a persistent internet connection to function.
* **Media Playback:** No audio players or audiobooks. The hardware is not built for this.
* **Complex Typing Apps:** The device lacks a physical keyboard, making long-form typing tedious. Apps should rely primarily on button-driven navigation, D-pads, and simple selections rather than extensive text entry.
* **PDF Rendering:** Fixed-layout pages have to be shown as images, which means constant panning and zooming. Out of scope on this hardware class, as upstream.

## 3. App Evaluation Guidelines

If you want to build an app for Teleport, ask yourself the following questions:

1. **Does it work offline?** (If it requires data, does it fetch it efficiently and cache it locally?)
2. **Does it respect the E-ink display?** (Does it avoid animations and unnecessary full-screen refreshes?)
3. **Is it entirely button-navigable?** (Does it have intuitive controls using the physical D-pad layout?)
4. **What does it displace?** (Flash and heap are both nearly spoken for; an addition is usually a trade.)

> **Note to Contributors:** If you have an idea for an app and are unsure if it fits the scope, please open a **Discussion** or issue before you start coding.

## 4. Hardware Targets

Teleport targets the **Xteink X4 (ESP32-C3)**. That is the only device it is built and tested on, and the `default` PlatformIO environment is the one we ship.

Upstream CrossPoint has broadened well past that — X3, Xteink X4 Pro, Seeed reTerminal Sticky and M5PaperMono (ESP32-S3), behind a HAL / SDK boundary. Those build environments come along with the engine and are kept buildable, but no Teleport app has been verified on them. Device-specific code belongs behind the HAL, same as upstream, so the merge stays cheap.

## 5. Relationship to CrossPoint

Teleport is a fork of [`zakerytclarke/crosspoint-reader-apps`](https://github.com/zakerytclarke/crosspoint-reader-apps),
itself a fork of the [CrossPoint engine](https://github.com/crosspoint-reader/crosspoint-reader).
We track the engine opportunistically — take its reading and rendering work,
decline its app-layer direction — and we ship our own firmware releases. The
device's update check points at Teleport's releases, not CrossPoint's; installing
theirs would replace every app on this page. See [docs/upstream.md](docs/upstream.md).

**Where the scopes diverge.** CrossPoint's stated scope rules out interactive apps, RSS readers and any new "talk to a server" connector, and its current focus is memory footprint, flash footprint, code cleanup and the multi-device port. Those exclusions are exactly Teleport's reason to exist, so the app layer, the Read Later / briefing networking and the Nest integration will never be upstreamable. Everything else — rendering, typography, fonts, library, settings, the web server, i18n — we want from upstream verbatim, and changes there should be written so they could be sent upstream rather than forked further.

**What this costs us at merge time.** Upstream's freeze on new network connectors and new themes means we should not add either casually: each one is a conflict we re-resolve on every engine merge. Prefer app-layer code under `src/activities/` and keep engine files as close to upstream as the feature allows.
