#pragma once
#include <stddef.h>
#include <stdio.h>

struct BatteryStatus {
  int presence = -1;
  int percent = -1;
  int charging = -1;
  static BatteryStatus decode(int reg00, int rawPercent, int reg01) {
    BatteryStatus result;
    result.presence = reg00 < 0 ? -1 : !!(reg00 & 0x08);
    if (result.presence == 1 && rawPercent >= 0 && rawPercent <= 100) result.percent = rawPercent;
    const int direction = reg01 < 0 ? -1 : (reg01 >> 5) & 3;
    result.charging = direction < 0 || direction == 3 ? -1 : direction == 1;
    return result;
  }
  bool operator==(const BatteryStatus &other) const {
    return presence == other.presence && percent == other.percent && charging == other.charging;
  }
  void text(char *buffer, size_t size) const {
    char battery[16];
    if (presence == 0) {
      snprintf(buffer, size, "NO BAT");
      return;
    }
    if (presence < 0 || percent < 0) snprintf(battery, sizeof(battery), "BAT ?%%");
    else snprintf(battery, sizeof(battery), "BAT %d%%", percent);
    snprintf(buffer, size, "%s  %s", battery,
             presence < 0 || charging < 0 ? "CHARGE UNKNOWN" : charging ? "CHARGING" : "NOT CHARGING");
  }
};
