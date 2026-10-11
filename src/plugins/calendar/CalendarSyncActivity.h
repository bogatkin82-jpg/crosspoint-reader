#pragma once

#include <string>
#include <vector>

#include "CalendarPlugin.h"
#include "activities/Activity.h"

namespace plugins::calendar {

// Downloads the plugin's internet calendars (iCal feeds) over Wi-Fi:
// connects with saved credentials (or shows the network picker), syncs the
// clock over NTP, streams each feed to "<file>.part", checks it is a
// VCALENDAR (a login/HTML page is rejected and the old file kept) and swaps it
// in. On exit, when this screen brought Wi-Fi up, the device silently restarts
// back into the plugin to return the TLS/Wi-Fi heap (same as other Wi-Fi
// screens).
class CalendarSyncActivity final : public Activity {
 public:
  CalendarSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Manifest& manifest,
                       std::vector<Subscription> subscriptions)
      : Activity("CalendarSync", renderer, mappedInput), manifest(manifest), subs(std::move(subscriptions)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  bool preventAutoSleep() override { return state == State::Syncing; }
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t { WaitingWifi, Syncing, Done };

  void runSync();
  bool syncOne(const Subscription& s, std::string& error);

  Manifest manifest;
  std::vector<Subscription> subs;
  State state = State::WaitingWifi;
  bool tearDownWifi = false;
  int okCount = 0;
  int current = 0;
  int progressPct = -1;
  std::string errorText;
};

}  // namespace plugins::calendar
