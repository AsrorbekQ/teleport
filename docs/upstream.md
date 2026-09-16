# Upstream: where Teleport comes from, and how to take updates

Teleport sits at the end of a three-repo chain. Knowing which link an "update"
comes from decides everything else on this page.

| Link | Repo | State (2026-09-16) |
| --- | --- | --- |
| Engine | [`crosspoint-reader/crosspoint-reader`](https://github.com/crosspoint-reader/crosspoint-reader) | Alive. `develop` at 1.6.5, release 1.6.0, pushed daily. Has no app layer at all. |
| Apps fork | [`zakerytclarke/crosspoint-reader-apps`](https://github.com/zakerytclarke/crosspoint-reader-apps) — git remote `origin` | Dormant. Tip `c623c22`, 2026-06-03. No tags, no releases. Added the app layer (`AppRegistry`, Calculator, Chess, Sudoku, Weather, Wikipedia, DuckDuckGo, RSS, Dice). |
| Teleport | [`AsrorbekQ/teleport`](https://github.com/AsrorbekQ/teleport) — git remote `github` | This repo. 29 commits on top of `c623c22`. |

Note the remote names are inverted from the usual convention: **`origin` is
upstream, `github` is ours.** Any sync snippet that says `git fetch upstream`
is wrong here.

## Updates are ours now

`OtaUpdater.cpp` used to check
`crosspoint-reader/crosspoint-reader/releases/latest`. The device reported
`1.3.0`, CrossPoint 1.6.0 ships an asset named exactly `firmware.bin`, and the
tag is bare `X.Y.Z` — so every gate in `isUpdateNewer()` passed and one
**Settings ▸ System ▸ Check for updates ▸ Confirm** would have flashed stock
CrossPoint over Teleport, with no rollback (`OtaBootSwitch` writes
`ESP_OTA_IMG_NEW`, never `PENDING_VERIFY`) and no working USB bootloader on this
unit. SD-card data survives such a flash, but nothing left on the device can
read it.

That check now points at Teleport's own releases, the version line starts at
`2.0.0` (above CrossPoint's 1.x, so their releases can never compare as newer
even if a merge puts their URL back), and a non-numeric tag is refused instead of
being compared against uninitialised stack.

### Cutting a release the device can install

```sh
git tag 2.0.1 && git push github 2.0.1
```

`.github/workflows/release.yml` builds `-e gh_release` and publishes the tag as
a GitHub Release with `firmware.bin` attached. Three constraints, all enforced
in that file or by the device:

- the tag must be **bare `X.Y.Z`** — `OtaUpdater` parses it with `%d.%d.%d`, and
  the workflow's tag filter will not fire on `v2.0.1`;
- the asset must be named exactly **`firmware.bin`** — `ReleaseJsonParser`
  `strcmp`s that name;
- `[crosspoint] version` in `platformio.ini` must match the tag — the workflow
  refuses to build otherwise, because a mismatched release can never satisfy its
  own update check.

Until the first tag is pushed, `/releases/latest` 404s and **Check for updates**
reports "Update failed" rather than "no update" — every non-OK result maps to the
same screen (`OtaUpdateActivity.cpp:31-38`). It fails closed; nothing is flashed.

## Absorbing an engine update

Measured against the engine's last commit before the apps fork (`34e923d`),
Teleport **deletes nothing** and adds 64 new files. The whole conflict surface is
44 engine files, of which 14 are code:

```
lib/Logging/{Logging.cpp,Logging.h}          platformio.ini
src/activities/boot_sleep/SleepActivity.{cpp,h}
src/activities/reader/TxtReaderActivity.cpp  src/main.cpp
src/components/themes/BaseTheme.h            src/components/themes/lyra/LyraTheme.cpp
src/network/CrossPointWebServer.{cpp,h}      src/network/HttpDownloader.{cpp,h}
src/images/Logo120.h (generated)
```

Everything expensive — `AppRegistry.cpp`, the `UIIcon` churn, the six deleted
apps, the 23 re-stamped translation YAMLs — belongs to the dormant apps fork, not
the engine, so it cannot conflict with an engine update.

Setup, once:

```sh
git config rerere.enabled true          # the same conflicts recur every cycle
git config merge.conflictstyle zdiff3
git remote add engine https://github.com/crosspoint-reader/crosspoint-reader.git
git tag upstream-absorbed c623c22       # what we have already taken
```

Then, per cycle: **merge, never rebase.** `master` is published to `github`, and
`AppRegistry.cpp` is touched by 7 of our 29 commits — a rebase re-resolves it up
to 7 times, a merge once.

```sh
git fetch engine
git log --oneline upstream-absorbed..engine/develop   # empty => nothing to do
git tag premerge-$(date +%F) master                   # escape hatch
git merge engine/develop
```

Conflict triage, mechanical first:

1. **Translation YAMLs** — take theirs, then
   `sed -i '' 's/^STR_CROSSPOINT:.*/STR_CROSSPOINT: "Teleport"/' lib/I18n/translations/*.yaml`
   and re-append `english.yaml`'s Teleport block (`STR_FLASHCARDS:` to EOF).
   The generated `I18nStrings.cpp` is gitignored and cannot conflict;
   `scripts/gen_i18n.py` exits 1 if a `STR_` key our C++ uses went missing, which
   is a free check that the merge dropped nothing.
2. **Deleted apps** — every modify/delete conflict stays deleted (print the list
   before piping it anywhere).
3. **`open-x4-sdk`** — always take upstream's pin, then
   `git submodule update --init --recursive && rm -rf .pio/libdeps`
   (PlatformIO consumes it via `symlink://` and caches stale `.pio-link` files).
4. **`Logo120.png` / `Logo120.h`** — regenerate with `scripts/make_logo.py`,
   don't hand-merge a binary.
5. **Code, by hand.** Three files look like pure `clang-format` reflow and are
   not: `TxtReaderActivity.cpp` hides the `/apps/readlater/` back-navigation
   condition, `RssActivity.h` the `sendToReadLater()`/`saveDiagnosticLog()`
   declarations, `DownloadWatchdog.cpp` the `kick()` used by `HttpDownloader`.
   Resolving any of them "take theirs" drops a feature silently.

Leave `core.hooksPath` unset while merging. `.githooks/pre-commit` calls
`bin/clang-format-fix` without `-g`, so it reformats all 370 tracked C/C++ files
— into the merge commit.

### Before a big jump, know this

The engine moved from platform `55.03.37` to `55.03.311` and now patches wolfSSL.
Our TLS work in `HttpDownloader.cpp` is mbedTLS-specific and hand-declares
`esp_crt_verify_callback`. Against current engine that file is a rewrite, not a
merge — and possibly a deletion, since several hundred engine commits may already
fix the redirect and IPv4 problems it works around. If the jump is ever worth
making, re-forking from the engine and copying our app directories across is
cheaper than merging through the dormant apps fork.

## Verifying a merge

```sh
pio run -t clean && pio run -e default
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
```

cppcheck ships inside PlatformIO, so `pio check` needs no install — but it is
already red: 45 pre-existing low-severity defects in RssActivity, LyraTheme,
HttpDownloader and the app dirs. The signal is "no *new* defects", not "passes".
Scope it to what you touched with `--pattern`. CI's `clang-format` and `cppcheck`
jobs are also red for pre-existing reasons (the fork's own files were never run
through clang-format 21), so a red CI badge is not by itself evidence about your
merge — read which job failed.

`cmake`, `ninja` and `ctest` are not installed, so the unit tests are CI-only.
Xcode ships clang-format 21 at
`/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang-format`,
which agrees with this repo's `.clang-format` per file but disagrees with the
tree at large (~980 violations across 77 files it has never formatted) — so run
it on the files you changed, never across the repo. No automated test covers
Flashcards, Briefing, Habits, Read Later, RSS, the web server or TLS — all of it
is hands-on.

Keep a known-good image on the SD card **before** flashing; the recovery picker
(hold Power + Up at boot) reads the card, so the rescue image has to already be
there.

```sh
cp .pio/build/default/firmware.bin ~/teleport-firmware/teleport-$(git rev-parse --short HEAD).bin
curl -F "file=@$HOME/teleport-firmware/known-good.bin" "http://crosspoint.local/upload?path=/firmware"
```

Nest talks to seven engine-owned endpoints — `/api/status`, `/api/files`,
`/download`, `/upload`, `/mkdir`, `/move`, `/delete` — with no fallback; only
`/api/readlater` degrades gracefully (it falls back to editing
`/apps/readlater/queue.txt`). A broken `/mkdir` is the one that hides: Nest's
`ensure_dir` swallows the HTTP error, so the send still reports success and the
failure only surfaces later as a failed `/upload`. Re-run
`.venv/bin/python -m pytest` in `teleport-nest` and send one URL end-to-end after
any merge that touches `CrossPointWebServer.cpp`.
