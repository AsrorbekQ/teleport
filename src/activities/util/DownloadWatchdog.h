#pragma once

namespace DownloadWatchdog {
extern bool gotTimeout;
void start(unsigned long timeoutMs = 15000);
void kick();  // restart the countdown; no-op when not running
void stop();
}  // namespace DownloadWatchdog
