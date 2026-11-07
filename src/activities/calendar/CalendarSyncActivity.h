#pragma once

#include <string>
#include <vector>

#include "CalendarData.h"
#include "activities/Activity.h"

namespace calendar {

// Downloads the configured internet calendars (iCal feeds) over Wi-Fi:
// connects with saved credentials (or shows the network picker), syncs the
// clock over NTP, streams each feed to "<file>.part", checks it is a
// VCALENDAR (a login/HTML page is rejected and the old file kept) and swaps it
// in. On exit, after the sync ran, the device silently restarts
// back into the calendar to return the TLS/Wi-Fi heap (same as other Wi-Fi
// screens).
class CalendarSyncActivity final : public Activity {
 public:
  CalendarSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Config& config,
                       std::vector<Subscription> subscriptions)
      : Activity("CalendarSync", renderer, mappedInput), config(config), subs(std::move(subscriptions)) {}

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

  Config config;
  std::vector<Subscription> subs;
  State state = State::WaitingWifi;
  bool tearDownWifi = false;
  int okCount = 0;
  int current = 0;
  int progressPct = -1;
  std::string errorText;
};

}  // namespace calendar
