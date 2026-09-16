# Project Vision & Scope: Teleport

The goal of **Teleport** is to create an efficient, open-source reading experience for the Xteink X4, augmented by a suite of high-quality, community-driven applications. We believe an e-reader can be a powerful, distraction-free companion device that goes beyond just reading books—provided the apps are designed with care for the e-ink display and battery life.

## 1. Core Mission

To provide a lightweight, high-performance firmware that maximizes the utility of the X4. We aim to support a thriving ecosystem of apps and tools while maintaining the device's exceptional legibility, long battery life, and distraction-free nature.

## 2. Scope

### In-Scope

*These are features and apps that directly improve the utility of the device while respecting its hardware constraints.*

* **Core Reading Experience:** The CrossPoint reading engine as forked — EPUB rendering, typography, hyphenation, library management, and local file transfer.
* **Productivity Utilities:** Offline tools that earn their place on a secondary, distraction-free screen — habit tracking, spaced-repetition flashcards, the sleep-screen briefing, clocks and dice rollers.
* **Offline-First Feeds & Articles:** RSS and Read Later. These MUST fetch via Wi-Fi only when requested, cache the text locally to the SD card, and then disconnect from the network to allow for battery-friendly offline reading.
* **Reference Tools:** Local dictionary lookup, offline documentation viewers, etc.

### Out-of-Scope

*These items are rejected because they compromise the device's stability, battery life, or e-ink constraints.*

* **High-Framerate / Animated Games:** E-ink displays have a slow refresh rate. Action games, platformers, or anything requiring rapid screen updates are fundamentally incompatible with this hardware.
* **Apps That Do Not Earn Their Flash:** The image already sits at 92% of the 6.5 MB app partition. Calculator, Weather, Chess, Sudoku, Wikipedia and DuckDuckGo were removed for this reason; new apps have to displace something.
* **Always-Online / Background Polling Apps:** Background Wi-Fi tasks rapidly drain the small battery and complicate the single-core CPU's execution. Apps must not poll servers continuously in the background or require a persistent internet connection to function.
* **Media Playback:** No Audio players or Audio-books. The hardware is not built for this.
* **Complex Typing Apps:** The device lacks a physical keyboard, making long-form typing tedious. Apps should rely primarily on button-driven navigation, D-pads, and simple selections rather than extensive text entry.

## 3. App Evaluation Guidelines

We warmly welcome community contributions! If you want to build an app for Teleport, ask yourself the following questions:

1. **Does it work offline?** (If it requires data, does it fetch it efficiently and cache it locally?)
2. **Does it respect the E-ink display?** (Does it avoid animations and unnecessary full-screen refreshes?)
3. **Is it entirely button-navigable?** (Does it have intuitive controls using the physical D-pad layout?)

> **Note to Contributors:** If you have an idea for an app and are unsure if it fits the scope, please open a **Discussion** or issue before you start coding! We are eager to help you design it to fit these guidelines.

## 4. Relationship to CrossPoint

Teleport is a fork of [`zakerytclarke/crosspoint-reader-apps`](https://github.com/zakerytclarke/crosspoint-reader-apps),
itself a fork of the [CrossPoint engine](https://github.com/crosspoint-reader/crosspoint-reader).
We track the engine opportunistically — take its reading and rendering work,
decline its app-layer direction — and we ship our own firmware releases. The
device's update check points at Teleport's releases, not CrossPoint's; installing
theirs would replace every app on this page. See [docs/upstream.md](docs/upstream.md).
