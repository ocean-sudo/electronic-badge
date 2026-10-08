#include "auto_sleep.h"
#include "power_off.h"

#include <cassert>
#include <iostream>

namespace {
struct FakePmu {
  int control = 0x70;
  int status = 0;
  int shutdownCalls = 0;
  bool readFails = false;
  bool statusReadFails = false;

  void setOnLevel(uint8_t option) { control |= option; }
  int readRegister(uint8_t address) const {
    if (readFails || (address == 0x10 && statusReadFails)) return -1;
    if (address == 0x27) return control;
    if (address == 0x10) return status;
    return -1;
  }
  void shutdown() { ++shutdownCalls; }
};

struct FakePeripherals {
  int displayShutdowns = 0;
  int touchShutdowns = 0;
  int gravityShutdowns = 0;
};

void batteryIdleUsesPmicShutdown() {
  AutoSleep idle;
  idle.batterySeconds = 5;
  idle.updateSupply(AutoSleep::Supply::Battery, 0);
  assert(idle.timeoutAction(4999) == AutoSleep::TimeoutAction::None);
  assert(idle.timeoutAction(5000) == AutoSleep::TimeoutAction::PmicPowerOff);

  FakePmu pmu;
  assert(PmicPowerOff::configureOnLevel(pmu, 0x27, 3));
  assert((pmu.control & 0x03) == 3);
  assert((pmu.control & 0xFC) == 0x70);
  const bool issued = idle.timeoutAction(5000) == AutoSleep::TimeoutAction::PmicPowerOff &&
                      PmicPowerOff::shutdownIfReady(pmu, true);
  assert(issued && pmu.shutdownCalls == 1);
  assert(!PmicPowerOff::shutdownIfReady(pmu, false) && pmu.shutdownCalls == 1);
}

void cutoverChecksBatteryBeforeChangingPeripherals() {
  FakePmu pmu;
  FakePeripherals peripherals;
  const auto prepare = [&] {
    ++peripherals.displayShutdowns;
    ++peripherals.touchShutdowns;
    ++peripherals.gravityShutdowns;
  };

  pmu.status = 0x00; // STATUS1: VBUS-good clear means battery.
  assert(PmicPowerOff::shutdownOnBattery(pmu, true, true, 0x10, prepare));
  assert(peripherals.displayShutdowns == 1 && peripherals.touchShutdowns == 1 &&
         peripherals.gravityShutdowns == 1 && pmu.shutdownCalls == 1);

  pmu.status = 0x20; // A newly good VBUS source aborts at cutover.
  assert(!PmicPowerOff::shutdownOnBattery(pmu, true, true, 0x10, prepare));
  assert(peripherals.displayShutdowns == 1 && peripherals.touchShutdowns == 1 &&
         peripherals.gravityShutdowns == 1 && pmu.shutdownCalls == 1);
  assert(!PmicPowerOff::shutdownOnBattery(pmu, false, true, 0x10, prepare));
  assert(peripherals.displayShutdowns == 1 && peripherals.touchShutdowns == 1 &&
         peripherals.gravityShutdowns == 1 && pmu.shutdownCalls == 1);
}

void failedOnLevelOrStatusReadbackBlocksShutdown() {
  FakePmu pmu;
  FakePeripherals peripherals;
  pmu.readFails = true;
  assert(!PmicPowerOff::configureOnLevel(pmu, 0x27, 3));
  assert(!PmicPowerOff::shutdownOnBattery(pmu, true, false, 0x10, [&] { ++peripherals.displayShutdowns; }));
  assert(peripherals.displayShutdowns == 0 && pmu.shutdownCalls == 0);
  pmu.readFails = false;
  pmu.statusReadFails = true;
  assert(!PmicPowerOff::shutdownOnBattery(pmu, true, true, 0x10, [&] { ++peripherals.displayShutdowns; }));
  assert(peripherals.displayShutdowns == 0 && pmu.shutdownCalls == 0);
}

void activeInputsDeferCutover() {
  PmicPowerOff::ShutdownInputs inputs{false, false, false, false, false, false, false};
  assert(PmicPowerOff::inputsIdle(inputs));
  inputs.fingerDown = true; assert(!PmicPowerOff::inputsIdle(inputs)); inputs.fingerDown = false;
  inputs.touchActive = true; assert(!PmicPowerOff::inputsIdle(inputs)); inputs.touchActive = false;
  inputs.powerKeyPressed = true; assert(!PmicPowerOff::inputsIdle(inputs)); inputs.powerKeyPressed = false;
  inputs.bootPressed = true; assert(!PmicPowerOff::inputsIdle(inputs)); inputs.bootPressed = false;
  inputs.commandActive = true; assert(!PmicPowerOff::inputsIdle(inputs)); inputs.commandActive = false;
  inputs.commandInvalid = true; assert(!PmicPowerOff::inputsIdle(inputs)); inputs.commandInvalid = false;
  inputs.serialInputPending = true; assert(!PmicPowerOff::inputsIdle(inputs));
}
} // namespace

int main() {
  batteryIdleUsesPmicShutdown();
  cutoverChecksBatteryBeforeChangingPeripherals();
  failedOnLevelOrStatusReadbackBlocksShutdown();
  activeInputsDeferCutover();
  std::cout << "battery PMIC power-off smoke passed\n";
}
