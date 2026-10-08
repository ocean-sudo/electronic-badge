#include "auto_sleep.h"

#include <cassert>
#include <iostream>

namespace {
void supplyAndTimeouts() {
  using Supply = AutoSleep::Supply;
  AutoSleep sleep;
  sleep.usbSeconds = 15;
  sleep.batterySeconds = 0;
  assert(AutoSleep::decodeSupply(-1) == Supply::Unknown);
  assert(AutoSleep::decodeSupply(0x08) == Supply::Battery);
  assert(AutoSleep::decodeSupply(0x20) == Supply::Usb);
  assert(AutoSleep::decodeSupply(0x28) == Supply::Usb);
  assert(sleep.sleepAction() == AutoSleep::TimeoutAction::ScreenOff);
  assert(sleep.due(15000));
  assert(sleep.timeoutAction(15000) == AutoSleep::TimeoutAction::ScreenOff);
  assert(sleep.updateSupply(Supply::Usb, 15000));
  assert(!sleep.due(15000));
  assert(sleep.sleepAction() == AutoSleep::TimeoutAction::ScreenOff);
  // A screen-off USB device must request PMIC shutdown when unplugged, even with battery timeout OFF.
  assert(sleep.updateSupply(Supply::Battery, 15001));
  assert(!sleep.due(15001));
  assert(sleep.sleepAction() == AutoSleep::TimeoutAction::PmicPowerOff);
  assert(sleep.updateSupply(Supply::Unknown, 15002));
  assert(sleep.sleepAction() == AutoSleep::TimeoutAction::ScreenOff);
  assert(sleep.timeoutSeconds() == 15);
  sleep.batterySeconds = 5;
  sleep.updateSupply(Supply::Battery, UINT32_MAX - 1000U);
  assert(!sleep.due(3998));
  assert(sleep.due(3999));
  assert(sleep.timeoutAction(3998) == AutoSleep::TimeoutAction::None);
  assert(sleep.timeoutAction(3999) == AutoSleep::TimeoutAction::PmicPowerOff);
  assert(AutoSleep::validTimeout(0) && AutoSleep::validTimeout(5) && AutoSleep::validTimeout(86400));
  assert(!AutoSleep::validTimeout(4) && !AutoSleep::validTimeout(86401));
}

void touchReleaseMustBeConfirmed() {
  bool suppressed = true, confirmedRelease = false;
  uint32_t lastPoll = 100;
  unsigned reads = 0;
  auto readReleased = [&] { ++reads; return confirmedRelease; };
  AutoSleep::pollTouchRelease(false, suppressed, 10000, lastPoll, readReleased);
  assert(suppressed && reads == 0 && lastPoll == 100);
  AutoSleep::pollTouchRelease(true, suppressed, 149, lastPoll, readReleased);
  assert(suppressed && reads == 0);
  AutoSleep::pollTouchRelease(true, suppressed, 150, lastPoll, readReleased);
  assert(suppressed && reads == 1);
  AutoSleep::pollTouchRelease(true, suppressed, 100000, lastPoll, readReleased);
  assert(suppressed && reads == 2); // Neither a long hold nor an unconfirmed read times out.
  confirmedRelease = true;
  AutoSleep::pollTouchRelease(true, suppressed, 100050, lastPoll, readReleased);
  assert(!suppressed && reads == 3);
  AutoSleep::pollTouchRelease(true, suppressed, 200000, lastPoll, readReleased);
  assert(reads == 3);
  suppressed = true;
  lastPoll = UINT32_MAX - 20U;
  AutoSleep::pollTouchRelease(true, suppressed, 28, lastPoll, readReleased);
  assert(suppressed && reads == 3);
  AutoSleep::pollTouchRelease(true, suppressed, 29, lastPoll, readReleased);
  assert(!suppressed && reads == 4);
}
} // namespace

int main() {
  supplyAndTimeouts();
  touchReleaseMustBeConfirmed();
  std::cout << "auto sleep policy tests passed\n";
}
