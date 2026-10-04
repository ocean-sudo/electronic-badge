#pragma once

#include <Arduino.h>
#include "SensorQMI8658.hpp"
#include "gravity_motion.h"
#include "pin_config.h"

// The vendor's getDataReady() masks -1 register errors into true, and its
// powerOn()/powerDown() return void. Keep transport failures observable.
class GravitySensor : private SensorQMI8658 {
 public:
  GravityMotion pose;
  bool ok = false;

  void begin() {
    initialized_ = SensorQMI8658::begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
    if (!initialized_) { fail("imu_unavailable"); return; }
    ok = true;
    if (!stopHardware()) fail("imu_power_down");
  }

  const char *available() const { return ok ? nullptr : (*pose.error ? pose.error : "imu_unavailable"); }

  void enable(bool wanted, bool prime = false) {
    if (wanted == running_) return;
    if (!wanted) {
      pose.suspend();
      running_ = false;
      if (initialized_ && !stopHardware()) fail("imu_power_down");
      return;
    }
    if (!ok) return;
    if (!comm->clrRegisterBit(QMI8658_REG_CTRL1, 1) ||
        !disableGyroscope() ||
        configAccelerometer(ACC_RANGE_4G, ACC_ODR_LOWPOWER_21Hz, LPF_OFF) != 0 ||
        !comm->clrRegisterBit(QMI8658_REG_CTRL5, 0) ||
        !enableAccelerometer()) {
      fail("imu_configuration");
      return;
    }
    running_ = true;
    pose.resume(millis());
    sampleReceived_ = false;
    // Consume any old sample, then wait at most three 21Hz sample periods on
    // wake/menu exit so the first image uses the current, not suspended pose.
    float x, y, z;
    if (!getAccelerometer(x, y, z)) { fail("imu_read"); return; }
    lastPoll_ = millis();
    if (prime) {
      const uint32_t started = millis();
      do { delay(5); poll(); } while (running_ && !sampleReceived_ && millis() - started < 150U);
      if (running_ && !sampleReceived_) fail("imu_stale");
    }
  }

  void poll() {
    if (!running_) return;
    const uint32_t now = millis();
    pose.tick(now);
    if (now - lastPoll_ < 20U) return;
    lastPoll_ = now;
    const int ready = comm->readRegister(QMI8658_REG_STATUS0);
    if (ready < 0) { fail("imu_read"); return; }
    if (!(ready & 1)) return;
    float x, y, z;
    if (!getAccelerometer(x, y, z)) { fail("imu_read"); return; }
    pose.sample(x, y, z, now);
    sampleReceived_ = true;
  }

 private:
  bool stopHardware() {
    const bool accelerometer = disableAccelerometer();
    const bool gyroscope = disableGyroscope();
    const bool poweredDown = comm->setRegisterBit(QMI8658_REG_CTRL1, 1);
    return accelerometer && gyroscope && poweredDown;
  }

  void fail(const char *reason) {
    ok = false;
    pose.fault(reason);
    running_ = false;
    // A transport failure must not prevent a best-effort sampling shutdown.
    if (initialized_) stopHardware();
  }

  bool initialized_ = false, running_ = false, sampleReceived_ = false;
  uint32_t lastPoll_ = 0;
};
