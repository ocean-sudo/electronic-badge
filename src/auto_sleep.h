#pragma once

#include <stdint.h>


struct AutoSleep {
  enum class Supply { Unknown, Usb, Battery };
  uint32_t usbSeconds = 15;
  uint32_t batterySeconds = 0;
  uint32_t lastActivity = 0;
  Supply supply = Supply::Unknown;

  static bool validTimeout(uint32_t seconds) {
    return seconds == 0 || (seconds >= 5 && seconds <= 86400);
  }

  static Supply decodeSupply(int status) {
    return status < 0 ? Supply::Unknown : (status & 0x20) ? Supply::Usb : Supply::Battery;
  }


  // Poll only an outstanding sleep-entry contact; elapsed time never releases it.
  template <typename ReadReleased>
  static void pollTouchRelease(bool sleeping, bool &suppressed, uint32_t now,
                               uint32_t &lastPoll, ReadReleased readReleased) {
    if (!sleeping || !suppressed || now - lastPoll < 50U) return;
    lastPoll = now;
    if (readReleased()) suppressed = false;
  }
  void activity(uint32_t now) { lastActivity = now; }

  bool updateSupply(Supply value, uint32_t now) {
    if (supply == value) return false;
    supply = value;
    activity(now);
    return true;
  }

  uint32_t timeoutSeconds() const {
    // Unknown supply uses the USB protection policy, never the battery exemption.
    return supply == Supply::Battery ? batterySeconds : usbSeconds;
  }

  uint32_t remainingMs(uint32_t now) const {
    const uint32_t timeout = timeoutSeconds() * 1000U;
    const uint32_t elapsed = now - lastActivity;
    return elapsed >= timeout ? 0 : timeout - elapsed;
  }

  bool due(uint32_t now) const {
    return timeoutSeconds() != 0 && remainingMs(now) == 0;
  }

  enum class TimeoutAction { None, ScreenOff, PmicPowerOff };

  TimeoutAction sleepAction() const {
    return supply == Supply::Battery ? TimeoutAction::PmicPowerOff : TimeoutAction::ScreenOff;
  }

  TimeoutAction timeoutAction(uint32_t now) const {
    return due(now) ? sleepAction() : TimeoutAction::None;
  }
};

