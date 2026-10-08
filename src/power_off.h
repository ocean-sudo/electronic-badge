#pragma once

#include <stdint.h>
#include "auto_sleep.h"

namespace PmicPowerOff {
constexpr uint16_t kPowerOnHoldMilliseconds = 2000;
constexpr uint8_t kOnLevelMask = 0x03;

struct ShutdownInputs {
  bool fingerDown;
  bool touchActive;
  bool powerKeyPressed;
  bool bootPressed;
  bool commandActive;
  bool commandInvalid;
  bool serialInputPending;
};

inline bool inputsIdle(const ShutdownInputs &inputs) {
  return !inputs.fingerDown && !inputs.touchActive && !inputs.powerKeyPressed &&
         !inputs.bootPressed && !inputs.commandActive && !inputs.commandInvalid &&
         !inputs.serialInputPending;
}
template <typename Pmu>
bool configureOnLevel(Pmu &pmu, uint8_t registerAddress, uint8_t twoSecondOption) {
  if (twoSecondOption != kOnLevelMask) return false;
  pmu.setOnLevel(twoSecondOption);
  const int setting = pmu.readRegister(registerAddress);
  return setting >= 0 && (setting & kOnLevelMask) == twoSecondOption;
}

template <typename Pmu>
bool shutdownIfReady(Pmu &pmu, bool onLevelVerified) {
  if (!onLevelVerified) return false;
  pmu.shutdown();
  return true;
}

template <typename Pmu, typename PrepareShutdown>
bool shutdownOnBattery(Pmu &pmu, bool pmuAvailable, bool onLevelVerified,
                       uint8_t statusRegister, PrepareShutdown prepareShutdown) {
  if (!pmuAvailable || !onLevelVerified) return false;
  const int status = pmu.readRegister(statusRegister);
  if (AutoSleep::decodeSupply(status) != AutoSleep::Supply::Battery) return false;
  prepareShutdown();
  return shutdownIfReady(pmu, onLevelVerified);
}
}  // namespace PmicPowerOff
