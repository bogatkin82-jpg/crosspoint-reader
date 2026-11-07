#include "CalendarSyncActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace plugins::calendar {

namespace {
// A feed must start with BEGIN:VCALENDAR (after an optional BOM/whitespace);
// anything else is usually an SSO login page or an error document.
bool looksLikeCalendar(const std::string& path) {
  HalFile f;
  if (!Storage.openFileForRead("CALSYNC", path, f)) return false;
  char head[64] = {};
  const int n = f.read(head, sizeof(head) - 1);
  if (n <= 0) return false;
  const char* p = head;
  if (static_cast<uint8_t>(p[0]) == 0xEF && static_cast<uint8_t>(p[1]) == 0xBB && static_cast<uint8_t>(p[2]) == 0xBF)
    p += 3;
  while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
  return strncasecmp(p, "BEGIN:VCALENDAR", 15) == 0;
}
}  // namespace

void CalendarSyncActivity::onEnter() {
  Activity::onEnter();
  if (subs.empty()) {
    state = State::Done;
    errorText = tr(STR_CAL_SYNC_NO_URL);
    requestUpdate();
    return;
  }
  // Tear the radio down on the way out even when it was already up: the
  // calendar is parsed again right after, and TLS memory must be gone by then.
  tearDownWifi = true;
  if (WiFi.status() == WL_CONNECTED) {
    state = State::Syncing;
    requestUpdate();
    return;
  }
  state = State::WaitingWifi;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                             state = State::Done;
                             errorText = tr(STR_CLOCK_SYNC_NO_WIFI);
                           } else {
                             state = State::Syncing;
                           }
                           requestUpdate();
                         });
}

void CalendarSyncActivity::onExit() {
  Activity::onExit();
  // Record the attempt even when the picker was cancelled or never connected,
  // so auto-sync does not retry (and reboot) on every calendar open.
  if (tearDownWifi) writeLastAttempt(manifest.dir, nowUtcEpoch());
  if (tearDownWifi && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    delay(30);
    silentRestartToCalendar(manifest.dir.c_str());
  }
}

bool CalendarSyncActivity::syncOne(const Subscription& s, std::string& error) {
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP || ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("CALSYNC", "Low heap for TLS: %u free, %u max block", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    error = tr(STR_CAL_SYNC_LOW_MEMORY);
    return false;
  }
  const std::string part = s.path + ".part";
  Storage.remove(part.c_str());
  progressPct = -1;
  const auto result = HttpDownloader::downloadToFile(s.url, part, [this](size_t done, size_t total) {
    // Repaint in 10% steps: each e-ink refresh costs ~0.5 s of transfer time.
    const int pct = total > 0 ? static_cast<int>(done * 100 / total) / 10 * 10 : -1;
    if (pct != progressPct) {
      progressPct = pct;
      requestUpdateAndWait();
    }
  });
  if (result != HttpDownloader::OK) {
    LOG_ERR("CALSYNC", "Download failed (%d)", static_cast<int>(result));
    Storage.remove(part.c_str());
    error = tr(STR_CAL_SYNC_FAIL);
    return false;
  }
  if (!looksLikeCalendar(part)) {
    LOG_ERR("CALSYNC", "Not a calendar: %s", part.c_str());
    Storage.remove(part.c_str());
    error = tr(STR_CAL_SYNC_NOT_ICS);
    return false;
  }
  const std::string bak = s.path + ".bak";
  Storage.remove(bak.c_str());
  const bool hadOld = Storage.exists(s.path.c_str());
  if (hadOld && !Storage.rename(s.path.c_str(), bak.c_str())) {
    Storage.remove(part.c_str());
    error = tr(STR_CAL_SAVE_FAILED);
    return false;
  }
  if (!Storage.rename(part.c_str(), s.path.c_str())) {
    if (hadOld) Storage.rename(bak.c_str(), s.path.c_str());
    error = tr(STR_CAL_SAVE_FAILED);
    return false;
  }
  Storage.remove(bak.c_str());
  return true;
}

void CalendarSyncActivity::runSync() {
  // The calendar shows wrong days with an unset clock; NTP is cheap here.
  if (halClock.isAvailable() && halClock.syncFromNTP()) {
    SETTINGS.clockHasBeenSynced = 1;
    SETTINGS.saveToFile();
  }
  okCount = 0;
  errorText.clear();
  for (current = 0; current < static_cast<int>(subs.size()); current++) {
    requestUpdateAndWait();
    std::string err;
    if (syncOne(subs[current], err)) {
      okCount++;
    } else if (errorText.empty()) {
      errorText = err;
    }
  }
  const int64_t now = nowUtcEpoch();
  if (okCount > 0) writeLastSync(manifest.dir, now);
  writeLastAttempt(manifest.dir, now);
  LOG_INF("CALSYNC", "Synced %d/%u feeds", okCount, static_cast<unsigned>(subs.size()));
  state = State::Done;
  requestUpdate();
}

void CalendarSyncActivity::loop() {
  if (state == State::Syncing) {
    requestUpdateAndWait();  // paint "Syncing..." before blocking on the network
    runSync();
    return;
  }
  if (state != State::Done) return;
  int x, y;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    ActivityResult r;
    r.isCancelled = okCount == 0;
    setResult(std::move(r));
    finish();
  }
}

void CalendarSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int midY = renderer.getScreenHeight() / 2;
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_CAL_SYNC));

  char line[64];
  switch (state) {
    case State::WaitingWifi:
    case State::Syncing:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CAL_SYNCING), true, EpdFontFamily::BOLD);
      if (subs.size() > 1 || progressPct >= 0) {
        if (progressPct >= 0) {
          snprintf(line, sizeof(line), "%d / %u  -  %d%%", current + 1, static_cast<unsigned>(subs.size()),
                   progressPct);
        } else {
          snprintf(line, sizeof(line), "%d / %u", current + 1, static_cast<unsigned>(subs.size()));
        }
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, line);
      }
      break;
    case State::Done:
      if (okCount > 0 && okCount == static_cast<int>(subs.size())) {
        renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CAL_SYNC_OK), true, EpdFontFamily::BOLD);
      } else {
        renderer.drawCenteredText(UI_12_FONT_ID, midY - 20,
                                  okCount > 0 ? tr(STR_CAL_SYNC_PARTIAL) : tr(STR_CAL_SYNC_FAIL), true,
                                  EpdFontFamily::BOLD);
        const std::string fitted =
            renderer.truncatedText(UI_10_FONT_ID, errorText.c_str(), width - 2 * metrics.contentSidePadding);
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, fitted.c_str());
      }
      break;
  }
  if (state == State::Done) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

}  // namespace plugins::calendar
